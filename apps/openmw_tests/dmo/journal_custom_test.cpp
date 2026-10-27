#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwdialogue/journalcustom.hpp>
#include <apps/openmw/mwdialogue/journalimp.hpp>
#include <apps/openmw/mwlua/corebindings.hpp>
#include <apps/openmw/mwlua/journalbindings_ext.hpp>
#include <apps/openmw/mwlua/localscripts.hpp>
#include <apps/openmw/mwworld/esmstore.hpp>
#include <apps/openmw/mwworld/globals.hpp>
#include <apps/openmw/mwworld/ptr.hpp>

#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/journalentry.hpp>
#include <components/esm3/queststate.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/lua/luastate.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    ESM::RefId id(std::string_view value)
    {
        return ESM::RefId::stringRefId(value);
    }

    template <class Container>
    std::vector<std::string> texts(const Container& entries)
    {
        std::vector<std::string> result;
        for (const auto& entry : entries)
            result.push_back(entry.mText);
        return result;
    }

    class JournalWithServices : public MWDialogue::Journal
    {
    public:
        MWWorld::Globals mGlobals;
        std::vector<std::pair<ESM::RefId, int>> mUpdates;
        int mMessages = 0;

    protected:
        int getCustomJournalGlobal(MWWorld::GlobalVariableName name) const override
        {
            return mGlobals[name].getInteger();
        }

        void customJournalQuestUpdated(const ESM::RefId& quest, int index) override
        {
            mUpdates.emplace_back(quest, index);
        }

        void showCustomJournalMessage() override { ++mMessages; }
    };

    class dmoJournalCustom : public testing::Test
    {
    protected:
        MWBase::Environment mEnvironment;
        MWWorld::ESMStore mStore;
        JournalWithServices mJournal;
        Loading::Listener mProgress;
        MWDialogue::CustomJournalOptions mOptions;
        std::vector<std::function<void()>> mActions;

        void SetUp() override
        {
            mEnvironment.setESMStore(mStore);
            mEnvironment.setJournal(mJournal);
            mOptions.mDay = 42;
            mOptions.mMonth = 7;
            mOptions.mDayOfMonth = 19;
            mOptions.mSilent = true;
            auto stream = std::make_unique<std::stringstream>();
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.save(*stream);
            for (const auto name : { "dayspassed", "month", "day" })
            {
                ESM::Global global;
                global.blank();
                global.mId = id(name);
                global.mValue = ESM::Variant(0);
                writer.startRecord(ESM::REC_GLOB);
                global.save(writer);
                writer.endRecord(ESM::REC_GLOB);
            }
            for (const auto& name : { "quest", "other", "topic" })
            {
                ESM::Dialogue dialogue;
                dialogue.blank();
                dialogue.mId = id(name);
                dialogue.mStringId = name;
                dialogue.mType = dialogue.mId == id("topic") ? ESM::Dialogue::Topic : ESM::Dialogue::Journal;
                writer.startRecord(ESM::REC_DIAL);
                dialogue.save(writer);
                writer.endRecord(ESM::REC_DIAL);
                ESM::DialInfo info;
                info.blank();
                info.mId = id(std::string(name) + ":vanilla");
                info.mData.mType = dialogue.mType;
                info.mData.mJournalIndex = 10;
                info.mResponse = "Vanilla";
                writer.startRecord(ESM::REC_INFO);
                info.save(writer);
                writer.endRecord(ESM::REC_INFO);
                if (dialogue.mType == ESM::Dialogue::Topic)
                {
                    info.mPrev = info.mId;
                    info.mId = id("dmo:j:topic");
                    writer.startRecord(ESM::REC_INFO);
                    info.save(writer);
                    writer.endRecord(ESM::REC_INFO);
                }
            }
            writer.close();
            ESM::ESMReader reader;
            reader.setIndex(0);
            reader.open(std::move(stream), "journal-custom-content");
            ESM::Dialogue* dialogue = nullptr;
            mStore.load(reader, &mProgress, dialogue);
            mStore.setUp();
            mJournal.mGlobals.fill(mStore);
            mJournal.mGlobals[MWWorld::Globals::sDaysPassed].setInteger(60);
            mJournal.mGlobals[MWWorld::Globals::sMonth].setInteger(8);
            mJournal.mGlobals[MWWorld::Globals::sDay].setInteger(9);
        }

        void bindJournal(sol::state_view lua)
        {
            sol::table player(lua, sol::create);
            MWLua::addJournalEntryBindings(player, lua, &mJournal);
            MWLua::addQuestListBindings(lua, &mJournal);
            auto quest = lua.new_usertype<MWLua::Quest>("Quest");
            MWLua::addCustomJournalEntryBinding(
                quest, [this](std::function<void()> action) { mActions.push_back(std::move(action)); });
            lua["globalQuests"] = MWLua::Quests(MWLua::GObject{});
            lua["localQuests"] = MWLua::Quests(MWLua::SelfObject(MWLua::LObject{}));
            lua["otherLocalQuests"] = MWLua::Quests(MWLua::LObject{});
            lua["journal"] = [this]() -> const MWBase::Journal* { return &mJournal; };
        }

        void add(std::string text, std::string_view quest = "quest")
        {
            mJournal.addCustomEntry(id(quest), std::move(text), mOptions, MWWorld::Ptr());
        }

        void seedVanilla(std::string_view quest, std::string text, bool topic = false, std::string_view info = {})
        {
            ESM::JournalEntry entry{};
            entry.mTopic = id(quest);
            entry.mInfo = info.empty() ? id(std::string(quest) + ":vanilla") : id(info);
            entry.mText = std::move(text);
            entry.mDay = 50;
            entry.mMonth = 8;
            entry.mDayOfMonth = 1;
            auto stream = std::make_unique<std::stringstream>();
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.save(*stream);
            const std::vector<int> types = topic
                ? std::vector<int>{ ESM::JournalEntry::Type_Topic }
                : std::vector<int>{ ESM::JournalEntry::Type_Journal, ESM::JournalEntry::Type_Quest };
            for (int type : types)
            {
                entry.mType = type;
                writer.startRecord(ESM::REC_JOUR);
                entry.save(writer);
                writer.endRecord(ESM::REC_JOUR);
            }
            writer.close();
            ESM::ESMReader reader;
            reader.open(std::move(stream), "journal-custom-vanilla");
            while (reader.hasMoreRecs())
            {
                reader.getRecName();
                reader.getRecHeader();
                mJournal.readRecord(reader, ESM::REC_JOUR);
            }
        }

        std::unique_ptr<std::stringstream> save()
        {
            auto stream = std::make_unique<std::stringstream>();
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.setRecordCount(static_cast<int>(mJournal.countSavedGameRecords()));
            writer.save(*stream);
            mJournal.write(writer, mProgress);
            EXPECT_EQ(writer.getRecordCount(), mJournal.countSavedGameRecords() + 1);
            writer.close();
            return stream;
        }
    };

    TEST(dmoJournalCustomPrefix, OnlyReservedStringPrefixIsTransient)
    {
        EXPECT_TRUE(MWDialogue::isCustomJournalEntry(id("dmo:j:1")));
        EXPECT_TRUE(MWDialogue::isCustomJournalEntry(id("dmo:j:")));
        EXPECT_FALSE(MWDialogue::isCustomJournalEntry(id("dmo:journal:1")));
        EXPECT_FALSE(MWDialogue::isCustomJournalEntry(id("prefix:dmo:j:1")));
        EXPECT_FALSE(MWDialogue::isCustomJournalEntry(id("dmo:j")));
        EXPECT_FALSE(MWDialogue::isCustomJournalEntry(ESM::RefId()));
        EXPECT_FALSE(MWDialogue::isCustomJournalEntry(ESM::RefId::generated(1)));
    }

    TEST(dmoJournalCustomEntry, RecordFreeConstructorKeepsTextActorAndDate)
    {
        MWDialogue::StampedJournalEntry entry(id("absent"), id("dmo:j:1"), "A note", 4, 6, 12, "Scribe");
        EXPECT_EQ(entry.mTopic, id("absent"));
        EXPECT_EQ(entry.mInfoId, id("dmo:j:1"));
        EXPECT_EQ(entry.getText(), "A note");
        EXPECT_EQ(entry.mActorName, "Scribe");
        EXPECT_EQ(entry.mDay, 4);
        EXPECT_EQ(entry.mMonth, 6);
        EXPECT_EQ(entry.mDayOfMonth, 12);
    }

    TEST_F(dmoJournalCustom, AddsToBothListsWithUniqueIdsAndSuppliedDate)
    {
        add("First");
        add("Second");
        const auto& entries = mJournal.getEntries();
        ASSERT_EQ(entries.size(), 2);
        ASSERT_EQ(mJournal.getQuests().at(id("quest")).size(), 2);
        EXPECT_EQ(texts(entries), (std::vector<std::string>{ "First", "Second" }));
        EXPECT_EQ(texts(mJournal.getQuests().at(id("quest"))), texts(entries));
        EXPECT_TRUE(MWDialogue::isCustomJournalEntry(entries[0].mInfoId));
        EXPECT_TRUE(MWDialogue::isCustomJournalEntry(entries[1].mInfoId));
        EXPECT_NE(entries[0].mInfoId, entries[1].mInfoId);
        EXPECT_EQ(entries[0].mInfoId, mJournal.getQuests().at(id("quest"))[0].mInfoId);
        EXPECT_EQ(entries[0].mDay, 42);
        EXPECT_EQ(entries[0].mMonth, 7);
        EXPECT_EQ(entries[0].mDayOfMonth, 19);
    }

    TEST_F(dmoJournalCustom, InsertsBeforeFirstMatchInBothLists)
    {
        seedVanilla("quest", "First vanilla");
        seedVanilla("quest", "Second vanilla");
        mOptions.mBefore = id("quest:vanilla");
        add("Inserted");
        add("Inserted next");
        const std::vector<std::string> expected{ "Inserted", "Inserted next", "First vanilla", "Second vanilla" };
        EXPECT_EQ(texts(mJournal.getEntries()), expected);
        EXPECT_EQ(texts(mJournal.getQuests().at(id("quest"))), expected);
    }

    TEST_F(dmoJournalCustom, MissingAndOmittedAnchorsAppendInBothLists)
    {
        seedVanilla("quest", "Vanilla");
        mOptions.mBefore = id("missing");
        add("Missing anchor");
        mOptions.mBefore = {};
        add("No anchor");
        const std::vector<std::string> expected{ "Vanilla", "Missing anchor", "No anchor" };
        EXPECT_EQ(texts(mJournal.getEntries()), expected);
        EXPECT_EQ(texts(mJournal.getQuests().at(id("quest"))), expected);
    }

    TEST_F(dmoJournalCustom, AnchorInAnotherQuestAppendsToOwnQuest)
    {
        seedVanilla("quest", "Own vanilla");
        seedVanilla("other", "Other vanilla");
        mOptions.mBefore = id("other:vanilla");
        add("Custom");
        EXPECT_EQ(texts(mJournal.getEntries()),
            (std::vector<std::string>{ "Own vanilla", "Custom", "Other vanilla" }));
        EXPECT_EQ(texts(mJournal.getQuests().at(id("quest"))),
            (std::vector<std::string>{ "Own vanilla", "Custom" }));
    }

    TEST_F(dmoJournalCustom, CanAnchorBeforeAnotherCustomEntry)
    {
        add("Later");
        mOptions.mBefore = mJournal.getEntries().front().mInfoId;
        add("Earlier");
        const std::vector<std::string> expected{ "Earlier", "Later" };
        EXPECT_EQ(texts(mJournal.getEntries()), expected);
        EXPECT_EQ(texts(mJournal.getQuests().at(id("quest"))), expected);
    }

    TEST_F(dmoJournalCustom, SilentRestorationDoesNotSetStageOrNotify)
    {
        mOptions.mStage = 90;
        add("Restored");
        EXPECT_EQ(mJournal.getJournalIndex(id("quest")), 0);
        EXPECT_FALSE(mJournal.getQuests().at(id("quest")).isFinished());
        EXPECT_EQ(mJournal.getEntries().size(), 1);
        EXPECT_TRUE(mJournal.mUpdates.empty());
        EXPECT_EQ(mJournal.mMessages, 0);
        // No LuaManager or WindowManager exists in this fixture.
    }

    TEST_F(dmoJournalCustom, EmptyTextAndUnexpandedVariablesAreStoredVerbatim)
    {
        add("");
        add("A literal %name");
        const std::vector<std::string> expected{ "", "A literal %name" };
        EXPECT_EQ(texts(mJournal.getEntries()), expected);
        EXPECT_EQ(texts(mJournal.getQuests().at(id("quest"))), expected);
    }

    TEST_F(dmoJournalCustom, MissingAndNonJournalRecordsRejectWithoutStartingQuest)
    {
        for (const std::string name : { "missing", "topic" })
        {
            try
            {
                add("Rejected", name);
                FAIL() << "Accepted " << name;
            }
            catch (const std::runtime_error& error)
            {
                EXPECT_NE(std::string(error.what()).find(name), std::string::npos);
            }
            EXPECT_TRUE(mJournal.getEntries().empty());
            EXPECT_TRUE(mJournal.getQuests().empty());
            EXPECT_EQ(mJournal.countSavedGameRecords(), 0);
        }
    }

    TEST_F(dmoJournalCustom, WriterAndCounterOmitBothCustomCopiesAndKeepVanillaAndQuestState)
    {
        EXPECT_EQ(mJournal.countSavedGameRecords(), 0);
        save();
        add("Custom only");
        EXPECT_EQ(mJournal.countSavedGameRecords(), 1); // quest state, no entries
        save();
        seedVanilla("quest", "Vanilla journal");
        seedVanilla("topic", "Vanilla topic", true);
        seedVanilla("topic", "Reserved topic entry", true, "dmo:j:topic");
        ASSERT_EQ(mJournal.getTopics().at(id("topic")).size(), 2);
        add("Another custom");
        EXPECT_EQ(mJournal.countSavedGameRecords(), 4);
        ESM::ESMReader reader;
        reader.open(save(), "journal-custom-save");
        EXPECT_EQ(reader.getRecordCount(), 4);
        int questStates = 0;
        std::vector<int> types;
        while (reader.hasMoreRecs())
        {
            const auto name = reader.getRecName();
            reader.getRecHeader();
            if (name == ESM::REC_QUES)
            {
                ESM::QuestState state;
                state.load(reader);
                EXPECT_EQ(state.mTopic, id("quest"));
                ++questStates;
            }
            else
            {
                ASSERT_EQ(name, ESM::REC_JOUR);
                ESM::JournalEntry entry;
                entry.load(reader);
                EXPECT_FALSE(MWDialogue::isCustomJournalEntry(entry.mInfo));
                EXPECT_TRUE(entry.mText == "Vanilla journal" || entry.mText == "Vanilla topic");
                types.push_back(entry.mType);
            }
        }
        EXPECT_EQ(questStates, 1);
        EXPECT_EQ(types, (std::vector<int>{ ESM::JournalEntry::Type_Quest,
                             ESM::JournalEntry::Type_Journal, ESM::JournalEntry::Type_Topic }));
    }

    TEST_F(dmoJournalCustom, ReaddingInWrittenOrderRestoresPositionAmongLaterVanillaEntries)
    {
        seedVanilla("quest", "Earlier vanilla");
        add("First custom");
        add("Second custom");
        seedVanilla("other", "Later vanilla");
        const auto expected = texts(mJournal.getEntries());
        const auto expectedQuest = texts(mJournal.getQuests().at(id("quest")));
        const auto firstId = mJournal.getEntries()[1].mInfoId;
        auto stream = save();
        mJournal.clear();
        ESM::ESMReader reader;
        reader.open(std::move(stream), "journal-custom-reload");
        while (reader.hasMoreRecs())
        {
            const auto name = reader.getRecName();
            reader.getRecHeader();
            if (name == ESM::REC_JOUR)
                mJournal.readRecord(reader, ESM::REC_JOUR);
            else
                reader.skipRecord(); // quest-state loading emits a LuaManager event
        }
        EXPECT_EQ(texts(mJournal.getEntries()),
            (std::vector<std::string>{ "Earlier vanilla", "Later vanilla" }));
        mOptions.mBefore = id("other:vanilla");
        add("First custom");
        add("Second custom");
        EXPECT_EQ(texts(mJournal.getEntries()), expected);
        EXPECT_EQ(texts(mJournal.getQuests().at(id("quest"))), expectedQuest);
        EXPECT_NE(mJournal.getEntries()[1].mInfoId, firstId);
        EXPECT_EQ(mJournal.getEntries()[1].mDay, 42);
        EXPECT_EQ(mJournal.getEntries()[1].mMonth, 7);
        EXPECT_EQ(mJournal.getEntries()[1].mDayOfMonth, 19);
    }

    TEST_F(dmoJournalCustom, CrossQuestReloadAnchorPreservesBothListsBeforeLaterOwnEntry)
    {
        add("C1");
        add("C2");
        seedVanilla("other", "V");
        seedVanilla("quest", "W");
        const auto expectedJournal = texts(mJournal.getEntries());
        const auto expectedQuest = texts(mJournal.getQuests().at(id("quest")));
        auto stream = save();
        mJournal.clear();
        ESM::ESMReader reader;
        reader.open(std::move(stream), "journal-interleaved-reload");
        while (reader.hasMoreRecs())
        {
            const auto name = reader.getRecName();
            reader.getRecHeader();
            if (name == ESM::REC_JOUR)
                mJournal.readRecord(reader, ESM::REC_JOUR);
            else
                reader.skipRecord();
        }
        EXPECT_EQ(texts(mJournal.getEntries()), (std::vector<std::string>{ "V", "W" }));
        mOptions.mBefore = id("other:vanilla");
        add("C1");
        add("C2");
        EXPECT_EQ(texts(mJournal.getEntries()), expectedJournal);
        EXPECT_EQ(texts(mJournal.getQuests().at(id("quest"))), expectedQuest);
        EXPECT_EQ(expectedQuest, (std::vector<std::string>{ "C1", "C2", "W" }));
    }

    TEST_F(dmoJournalCustom, BindingRaisesNamedLuaErrorBeforeQueuingForInvalidQuest)
    {
        LuaUtil::LuaState lua{ nullptr, nullptr };
        std::string error;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["addCustom"] = [&](std::string quest, std::string text) {
                MWLua::makeCustomJournalEntryAction({ id(quest), true, true }, std::move(text), sol::nullopt);
            };
            auto result = view.sol().safe_script(R"(
                for _, quest in ipairs({'missing', 'topic'}) do
                    local ok, err = pcall(addCustom, quest, 'Rejected')
                    assert(not ok and string.find(err, quest, 1, true))
                end
            )", sol::script_pass_on_error);
            if (!result.valid())
                error = result.get<sol::error>().what();
        });
        EXPECT_TRUE(error.empty()) << error;
        EXPECT_TRUE(mJournal.getEntries().empty());
        EXPECT_TRUE(mJournal.getQuests().empty());
    }

    TEST_F(dmoJournalCustom, SharedBindingsKeepGlobalPermissionThroughLookupAndIteration)
    {
        mJournal.getOrStartQuest(id("quest"));
        LuaUtil::LuaState lua{ nullptr, nullptr };
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            bindJournal(view.sol());
            auto result = view.sol().safe_script(R"(
                globalQuests.quest:addCustomJournalEntry('Global lookup')
                for _, quest in pairs(globalQuests) do
                    quest:addCustomJournalEntry('Global iteration')
                end
                for _, quests in ipairs({localQuests, otherLocalQuests}) do
                    local ok, err = pcall(function() quests.quest:addCustomJournalEntry('Rejected') end)
                    assert(not ok and string.find(err, 'global scripts', 1, true))
                    for _, quest in pairs(quests) do
                        ok, err = pcall(function() quest:addCustomJournalEntry('Rejected') end)
                        assert(not ok and string.find(err, 'global scripts', 1, true))
                    end
                end
            )", sol::script_pass_on_error);
            ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
        });
        ASSERT_EQ(mActions.size(), 2);
        EXPECT_TRUE(mJournal.getEntries().empty());
        for (const auto& action : mActions)
            action();
        EXPECT_EQ(texts(mJournal.getEntries()),
            (std::vector<std::string>{ "Global lookup", "Global iteration" }));
    }

    TEST_F(dmoJournalCustom, BindingRejectsOutOfRangeMonthsBeforeQueuing)
    {
        LuaUtil::LuaState lua{ nullptr, nullptr };
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            for (int month : { 0, 13 })
            {
                sol::table options = view.sol().create_table_with("month", month);
                EXPECT_THROW(MWLua::makeCustomJournalEntryAction({ id("quest"), true, true }, "Rejected", options),
                    std::runtime_error);
            }
        });
        EXPECT_TRUE(mJournal.getEntries().empty());
    }

    TEST_F(dmoJournalCustom, BindingDefersStageAndCurrentDateUntilActionWithoutChangingFinishedState)
    {
        mJournal.getOrStartQuest(id("quest")).setFinished(true);
        LuaUtil::LuaState lua{ nullptr, nullptr };
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            bindJournal(view.sol());
            auto result = view.sol().safe_script(
                "globalQuests.quest:addCustomJournalEntry('Today', {stage = 23})", sol::script_pass_on_error);
            ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
        });
        ASSERT_EQ(mActions.size(), 1);
        EXPECT_TRUE(mJournal.getEntries().empty());
        EXPECT_EQ(mJournal.getJournalIndex(id("quest")), 0);
        EXPECT_TRUE(mJournal.mUpdates.empty());
        EXPECT_EQ(mJournal.mMessages, 0);
        mJournal.mGlobals[MWWorld::Globals::sDaysPassed].setInteger(100);
        mJournal.mGlobals[MWWorld::Globals::sMonth].setInteger(1);
        mJournal.mGlobals[MWWorld::Globals::sDay].setInteger(3);
        mActions.front()();
        ASSERT_EQ(mJournal.getEntries().size(), 1);
        EXPECT_EQ(mJournal.getJournalIndex(id("quest")), 23);
        EXPECT_TRUE(mJournal.getQuests().at(id("quest")).isFinished());
        EXPECT_EQ(mJournal.mUpdates, (std::vector<std::pair<ESM::RefId, int>>{ { id("quest"), 23 } }));
        EXPECT_EQ(mJournal.mMessages, 1);
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            auto result = view.sol().safe_script(R"(
                local entry = journal().journalTextEntries[1]
                assert(entry.text == 'Today' and entry.day == 100 and entry.month == 2 and entry.dayOfMonth == 3)
                globalQuests.quest:addCustomJournalEntry('Restored', {stage = 99, silent = true})
            )", sol::script_pass_on_error);
            ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
        });
        ASSERT_EQ(mActions.size(), 2);
        mActions.back()();
        EXPECT_EQ(mJournal.getJournalIndex(id("quest")), 23);
        EXPECT_TRUE(mJournal.getQuests().at(id("quest")).isFinished());
        EXPECT_EQ(mJournal.mUpdates.size(), 1);
        EXPECT_EQ(mJournal.mMessages, 1);
    }

    TEST_F(dmoJournalCustom, SuppliedLuaDateRoundTripsThroughJournalTextEntries)
    {
        LuaUtil::LuaState lua{ nullptr, nullptr };
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            bindJournal(view.sol());
            auto result = view.sol().safe_script(R"(
                globalQuests.quest:addCustomJournalEntry('Dated', {day = 43, month = 8, dayOfMonth = 13})
            )", sol::script_pass_on_error);
            ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
        });
        ASSERT_EQ(mActions.size(), 1);
        EXPECT_TRUE(mJournal.getEntries().empty());
        EXPECT_TRUE(mJournal.getQuests().empty());
        mActions.front()();
        ASSERT_EQ(mJournal.getEntries().size(), 1);
        EXPECT_EQ(mJournal.getEntries().front().mMonth, 7);
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            auto result = view.sol().safe_script(R"(
                local entry = journal().journalTextEntries[1]
                assert(entry.day == 43 and entry.month == 8 and entry.dayOfMonth == 13)
            )", sol::script_pass_on_error);
            ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
        });
        EXPECT_EQ(mJournal.getJournalIndex(id("quest")), 0);
        EXPECT_FALSE(mJournal.getQuests().at(id("quest")).isFinished());
        EXPECT_TRUE(mJournal.mUpdates.empty());
        EXPECT_EQ(mJournal.mMessages, 1);
    }

    TEST(dmoJournalCustomBindings, ExtensionIsAdvertisedAndReadOnly)
    {
        LuaUtil::LuaState lua{ nullptr, nullptr };
        std::string error;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["extensions"] = MWLua::initCoreApiExtensions(view.sol());
            auto result = view.sol().safe_script(R"(
                assert(extensions.journal_custom == 1)
                assert(extensions.watch == 1 and extensions.ipc == 1 and extensions.dialogue == 1)
                assert(not pcall(function() extensions.journal_custom = 2 end))
            )", sol::script_pass_on_error);
            if (!result.valid())
                error = result.get<sol::error>().what();
        });
        ASSERT_TRUE(error.empty()) << error;
    }

    TEST(dmoApiExtensions, EventsNameTheBackportedHooksAndAreReadOnly)
    {
        LuaUtil::LuaState lua{ nullptr, nullptr };
        std::string error;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["extensions"] = MWLua::initCoreApiExtensions(view.sol());
            auto result = view.sol().safe_script(R"(
                local names = {}
                for _, name in ipairs(extensions.events) do
                    names[#names + 1] = name
                end
                assert(#names == 2 and names[1] == "onDropped" and names[2] == "onPlaced")
                assert(#extensions.events == 2)
                assert(not pcall(function() extensions.events[3] = "onActivate" end))
                assert(not pcall(function() extensions.events = {} end))
            )", sol::script_pass_on_error);
            if (!result.valid())
                error = result.get<sol::error>().what();
        });
        ASSERT_TRUE(error.empty()) << error;
    }
}
