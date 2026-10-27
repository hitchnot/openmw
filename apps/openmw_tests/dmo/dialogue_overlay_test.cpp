#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwdialogue/dialogueoverlay.hpp>
#include <apps/openmw/mwlua/recordsbindings_ext.hpp>
#include <apps/openmw/mwworld/esmstore.hpp>
#include <apps/openmw/mwworld/store.hpp>

#include <components/esm/defs.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/formatversion.hpp>
#include <components/esm3/loaddial.hpp>
#include <components/esm3/loadinfo.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/lua/luastate.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using MWDialogue::InfoAnchor;

    ESM::RefId id(std::string_view value)
    {
        return ESM::RefId::stringRefId(value);
    }

    ESM::DialInfo draft(std::string_view infoId, std::string text)
    {
        ESM::DialInfo info;
        info.blank();
        info.mId = id(infoId);
        info.mResponse = std::move(text);
        return info;
    }

    std::vector<std::string> ids(const ESM::Dialogue& dialogue)
    {
        std::vector<std::string> result;
        for (const ESM::DialInfo& info : dialogue.mInfo)
            result.push_back(info.mId.serializeText());
        return result;
    }

    class dmoDialogueOverlay : public testing::Test
    {
    protected:
        MWBase::Environment mEnvironment;
        MWWorld::ESMStore mStore;
        Loading::Listener mProgress;

        MWWorld::Store<ESM::Dialogue>& dialogues() { return mStore.getWritable<ESM::Dialogue>(); }

        void SetUp() override
        {
            mEnvironment.setESMStore(mStore);
            auto stream = std::make_unique<std::stringstream>();
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.save(*stream);
            for (const auto& [name, type] : { std::pair{ "background", ESM::Dialogue::Topic },
                     std::pair{ "vanilla_quest", ESM::Dialogue::Journal } })
            {
                ESM::Dialogue dialogue;
                dialogue.blank();
                dialogue.mId = id(name);
                dialogue.mStringId = name;
                dialogue.mType = type;
                writer.startRecord(ESM::REC_DIAL);
                dialogue.save(writer);
                writer.endRecord(ESM::REC_DIAL);
                ESM::RefId previous;
                for (const char* suffix : { ":first", ":second" })
                {
                    ESM::DialInfo info = draft(std::string(name) + suffix, std::string("Vanilla") + suffix);
                    info.mPrev = previous;
                    info.mData.mType = type;
                    info.mData.mJournalIndex = 10;
                    writer.startRecord(ESM::REC_INFO);
                    info.save(writer);
                    writer.endRecord(ESM::REC_INFO);
                    previous = info.mId;
                }
                if (type == ESM::Dialogue::Topic)
                {
                    // A content file's own info whose id happens to carry DMO's prefix.
                    ESM::DialInfo info = draft("dmo:content:1", "Content");
                    info.mPrev = previous;
                    info.mData.mType = type;
                    writer.startRecord(ESM::REC_INFO);
                    info.save(writer);
                    writer.endRecord(ESM::REC_INFO);
                }
            }
            writer.close();
            ESM::ESMReader reader;
            reader.setIndex(0);
            reader.open(std::move(stream), "dialogue-overlay-content");
            ESM::Dialogue* dialogue = nullptr;
            mStore.load(reader, &mProgress, dialogue);
            mStore.setUp();
        }

        std::unique_ptr<std::stringstream> save()
        {
            auto stream = std::make_unique<std::stringstream>();
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.setRecordCount(static_cast<int>(dialogues().countOverlayRecords()));
            writer.save(*stream);
            dialogues().writeOverlay(writer);
            EXPECT_EQ(static_cast<std::size_t>(writer.getRecordCount()), dialogues().countOverlayRecords() + 1);
            writer.close();
            return stream;
        }

        std::vector<std::uint32_t> load(std::unique_ptr<std::stringstream> stream)
        {
            std::vector<std::uint32_t> names;
            ESM::ESMReader reader;
            reader.open(std::move(stream), "dialogue-overlay-save");
            while (reader.hasMoreRecs())
            {
                const auto name = reader.getRecName();
                reader.getRecHeader();
                names.push_back(name.toInt());
                EXPECT_TRUE(mStore.readRecord(reader, name.toInt()));
            }
            return names;
        }
    };

    TEST(dmoDialogueOverlayAnchor, RoundTripsAndRefusesUnknownForms)
    {
        for (const std::string_view text : { "after:dmo:a:1", "before:background:first", "front", "end" })
            EXPECT_EQ(InfoAnchor::parse(text)->toString(), text);
        EXPECT_FALSE(InfoAnchor::parse("after:"));
        EXPECT_FALSE(InfoAnchor::parse("middle"));
    }

    TEST_F(dmoDialogueOverlay, CreatesDialoguesAndPlacesInfosByAnchor)
    {
        const std::uint64_t generation = dialogues().getGeneration();
        const ESM::Dialogue& topic = dialogues().createDialogue(id("the ring"), "The Ring", ESM::Dialogue::Topic);
        EXPECT_NE(dialogues().getGeneration(), generation);
        EXPECT_EQ(dialogues().search(id("the ring")), &topic);
        EXPECT_EQ(topic.mStringId, "The Ring");
        dialogues().insertInfo(id("the ring"), draft("dmo:a:1", "One"), InfoAnchor{ InfoAnchor::Kind::End, {} });
        dialogues().insertInfo(id("the ring"), draft("dmo:a:2", "Two"), InfoAnchor{ InfoAnchor::Kind::Front, {} });
        dialogues().insertInfo(
            id("the ring"), draft("dmo:a:3", "Three"), InfoAnchor{ InfoAnchor::Kind::After, id("dmo:a:2") });
        dialogues().insertInfo(
            id("the ring"), draft("dmo:a:4", "Four"), InfoAnchor{ InfoAnchor::Kind::Before, id("dmo:a:1") });
        EXPECT_EQ(ids(topic), (std::vector<std::string>{ "dmo:a:2", "dmo:a:3", "dmo:a:4", "dmo:a:1" }));
        EXPECT_EQ(topic.mInfo.front().mData.mType, ESM::Dialogue::Topic);
        EXPECT_EQ(std::next(topic.mInfo.begin())->mPrev, id("dmo:a:2"));
        EXPECT_EQ(std::next(topic.mInfo.begin())->mNext, id("dmo:a:1"));
        EXPECT_TRUE(MWDialogue::DialogueOverlay::isOwned(topic.mInfo.front().mId));
        EXPECT_FALSE(MWDialogue::DialogueOverlay::isOwned(id("background:first")));

        const auto deltas = dialogues().getOverlay().deltas();
        ASSERT_EQ(deltas.size(), 5);
        EXPECT_EQ(deltas[0].mDialogue, id("the ring"));
        EXPECT_TRUE(deltas[0].mInfo.empty());
        EXPECT_TRUE(deltas[0].mConflict.empty());
        EXPECT_EQ(deltas[1].mInfo, id("dmo:a:1"));
    }

    TEST_F(dmoDialogueOverlay, RefusesWhatTheIdAndAnchorRulesForbid)
    {
        const auto refuses = [&](const std::function<void()>& call) { EXPECT_THROW(call(), std::invalid_argument); };
        refuses([&] { dialogues().createDialogue(id("ring"), "The Ring", ESM::Dialogue::Topic); });
        refuses([&] { dialogues().createDialogue(id("dmo:the ring"), "dmo:the ring", ESM::Dialogue::Topic); });
        refuses([&] { dialogues().createDialogue(id("quest"), "Quest", ESM::Dialogue::Journal); });
        refuses([&] { dialogues().createDialogue(id("greet"), "Greet", ESM::Dialogue::Greeting); });
        refuses([&] { dialogues().createDialogue(id("dmo:q:x"), "X", ESM::Dialogue::Voice); });
        refuses([&] { dialogues().createDialogue(id("vanilla_quest"), "vanilla_quest", ESM::Dialogue::Topic); });
        dialogues().createDialogue(id("dmo:q:letter"), "A Letter", ESM::Dialogue::Journal);
        const InfoAnchor end{ InfoAnchor::Kind::End, {} };
        refuses([&] { dialogues().insertInfo(id("dmo:q:letter"), draft("plain", "No prefix"), end); });
        refuses([&] { dialogues().insertInfo(id("vanilla_quest"), draft("dmo:a:1", "Vanilla"), end); });
        refuses([&] { dialogues().insertInfo(id("absent"), draft("dmo:a:1", "Absent"), end); });
        refuses([&] {
            dialogues().insertInfo(
                id("dmo:q:letter"), draft("dmo:a:1", "Lost"), InfoAnchor{ InfoAnchor::Kind::After, id("dmo:a:9") });
        });
        refuses([&] { dialogues().insertInfo(id("dmo:q:letter"), draft("dmo:j:1", "Journal prefix"), end); });
        dialogues().insertInfo(id("dmo:q:letter"), draft("dmo:a:1", "Once"), end);
        refuses([&] { dialogues().insertInfo(id("dmo:q:letter"), draft("dmo:a:1", "Twice"), end); });
        EXPECT_EQ(dialogues().getOverlay().deltas().size(), 2);
    }

    TEST_F(dmoDialogueOverlay, ContentTopicOfTheSameNameTakesInfosAsAConflict)
    {
        const ESM::Dialogue& background = *dialogues().search(id("background"));
        const std::vector<std::string> pristine = ids(background);
        EXPECT_EQ(&dialogues().createDialogue(id("background"), "background", ESM::Dialogue::Topic), &background);
        const ESM::DialInfo& inserted = dialogues().insertInfo(
            id("background"), draft("dmo:a:1", "Mine"), InfoAnchor{ InfoAnchor::Kind::Front, {} });
        const auto deltas = dialogues().getOverlay().deltas();
        ASSERT_EQ(deltas.size(), 2);
        EXPECT_EQ(deltas[0].mConflict, "content-file topic");

        dialogues().clearDynamic();
        EXPECT_EQ(dialogues().search(id("background")), &background);
        EXPECT_EQ(ids(background), pristine);
        EXPECT_EQ(pristine.back(), "dmo:content:1");
        EXPECT_EQ(inserted.mResponse, "Mine");
        EXPECT_TRUE(dialogues().getOverlay().deltas().empty());
    }

    TEST_F(dmoDialogueOverlay, ClearDynamicRemovesCreatedRecordsAndKeepsThemReadable)
    {
        const ESM::Dialogue& topic = dialogues().createDialogue(id("the ring"), "The Ring", ESM::Dialogue::Topic);
        const ESM::DialInfo& info = dialogues().insertInfo(
            id("the ring"), draft("dmo:a:1", "Kept for Lua"), InfoAnchor{ InfoAnchor::Kind::End, {} });
        const std::size_t size = dialogues().getSize();
        const std::uint64_t generation = dialogues().getGeneration();

        dialogues().clearDynamic();
        EXPECT_EQ(dialogues().search(id("the ring")), nullptr);
        EXPECT_EQ(dialogues().getSize(), size - 1);
        EXPECT_NE(dialogues().getGeneration(), generation);
        EXPECT_EQ(dialogues().countOverlayRecords(), 0);
        EXPECT_EQ(topic.mStringId, "The Ring");
        EXPECT_EQ(info.mResponse, "Kept for Lua");
        for (const ESM::Dialogue& dialogue : dialogues())
            EXPECT_NE(&dialogue, &topic);

        // A second creation after the restore is a new record, not the graveyard's.
        EXPECT_NE(&dialogues().createDialogue(id("the ring"), "The Ring", ESM::Dialogue::Topic), &topic);
    }

    TEST_F(dmoDialogueOverlay, SaveRecordsRoundTripThroughTheStore)
    {
        dialogues().createDialogue(id("the ring"), "The Ring", ESM::Dialogue::Topic);
        dialogues().createDialogue(id("dmo:q:letter"), "A Letter", ESM::Dialogue::Journal);
        dialogues().createDialogue(id("background"), "background", ESM::Dialogue::Topic);
        dialogues().insertInfo(id("the ring"), draft("dmo:a:1", "First"), InfoAnchor{ InfoAnchor::Kind::End, {} });
        dialogues().insertInfo(
            id("the ring"), draft("dmo:a:2", "Before"), InfoAnchor{ InfoAnchor::Kind::Before, id("dmo:a:1") });
        ESM::DialInfo stage = draft("dmo:a:3", "The letter arrived.");
        stage.mData.mJournalIndex = 10;
        stage.mQuestStatus = ESM::DialInfo::QS_Name;
        stage.mActor = id("fargoth");
        ESM::DialogueCondition condition;
        condition.mFunction = ESM::DialogueCondition::Function_Journal;
        condition.mVariable = "ms_fargothring";
        condition.mComparison = ESM::DialogueCondition::Comp_Ge;
        condition.mValue = 10;
        stage.mSelects.push_back(condition);
        dialogues().insertInfo(id("dmo:q:letter"), stage, InfoAnchor{ InfoAnchor::Kind::End, {} });
        dialogues().insertInfo(id("background"), draft("dmo:a:4", "Appended"),
            InfoAnchor{ InfoAnchor::Kind::After, id("background:first") });
        ASSERT_EQ(dialogues().countOverlayRecords(), 7);

        auto stream = save();
        dialogues().clearDynamic();
        ASSERT_EQ(dialogues().search(id("the ring")), nullptr);
        const auto names = load(std::move(stream));

        EXPECT_EQ(names,
            (std::vector<std::uint32_t>{ ESM::REC_DMDL, ESM::REC_DMDL, ESM::REC_DMDL, ESM::REC_DMIF, ESM::REC_DMIF,
                ESM::REC_DMIF, ESM::REC_DMIF }));
        const ESM::Dialogue* ring = dialogues().search(id("the ring"));
        ASSERT_NE(ring, nullptr);
        EXPECT_EQ(ring->mStringId, "The Ring");
        EXPECT_EQ(ids(*ring), (std::vector<std::string>{ "dmo:a:2", "dmo:a:1" }));
        const ESM::Dialogue* letter = dialogues().search(id("dmo:q:letter"));
        ASSERT_NE(letter, nullptr);
        ASSERT_EQ(letter->mInfo.size(), 1);
        const ESM::DialInfo& read = letter->mInfo.front();
        EXPECT_EQ(read.mResponse, "The letter arrived.");
        EXPECT_EQ(read.mData.mJournalIndex, 10);
        EXPECT_EQ(read.mQuestStatus, ESM::DialInfo::QS_Name);
        EXPECT_EQ(read.mActor, id("fargoth"));
        ASSERT_EQ(read.mSelects.size(), 1);
        EXPECT_EQ(read.mSelects[0].mFunction, ESM::DialogueCondition::Function_Journal);
        EXPECT_EQ(read.mSelects[0].mVariable, "ms_fargothring");
        EXPECT_EQ(ids(*dialogues().search(id("background"))),
            (std::vector<std::string>{ "background:first", "dmo:a:4", "background:second", "dmo:content:1" }));
        const auto deltas = dialogues().getOverlay().deltas();
        ASSERT_EQ(deltas.size(), 7);
        EXPECT_EQ(deltas[2].mConflict, "content-file topic");
    }

    TEST_F(dmoDialogueOverlay, FillJournalWalksTheLiveInfoLists)
    {
        dialogues().createDialogue(id("dmo:q:letter"), "A Letter", ESM::Dialogue::Journal);
        ESM::DialInfo name = draft("dmo:a:1", "A Letter");
        name.mQuestStatus = ESM::DialInfo::QS_Name;
        dialogues().insertInfo(id("dmo:q:letter"), name, InfoAnchor{ InfoAnchor::Kind::End, {} });
        ESM::DialInfo stage = draft("dmo:a:2", "It arrived.");
        stage.mData.mJournalIndex = 20;
        dialogues().insertInfo(id("dmo:q:letter"), stage, InfoAnchor{ InfoAnchor::Kind::End, {} });
        dialogues().createDialogue(id("the ring"), "The Ring", ESM::Dialogue::Topic);
        dialogues().insertInfo(id("the ring"), draft("dmo:a:3", "Ring"), InfoAnchor{ InfoAnchor::Kind::End, {} });
        // The order kept from loading never sees DMO's infos.
        EXPECT_TRUE(dialogues().search(id("dmo:q:letter"))->mInfoOrder.getOrderedInfo().empty());

        std::vector<std::string> entries, topicInfos, topics;
        MWDialogue::walkFillJournal(
            dialogues(),
            [&](const ESM::Dialogue& dialogue, const ESM::DialInfo& info) {
                entries.push_back(dialogue.mId.serializeText() + "/" + info.mId.serializeText());
            },
            [&](const ESM::Dialogue&, const ESM::DialInfo& info) { topicInfos.push_back(info.mId.serializeText()); },
            [&](const ESM::Dialogue& dialogue) { topics.push_back(dialogue.mId.serializeText()); });
        EXPECT_NE(std::find(entries.begin(), entries.end(), "dmo:q:letter/dmo:a:2"), entries.end());
        EXPECT_EQ(std::find(entries.begin(), entries.end(), "dmo:q:letter/dmo:a:1"), entries.end());
        EXPECT_NE(std::find(entries.begin(), entries.end(), "vanilla_quest/vanilla_quest:first"), entries.end());
        EXPECT_NE(std::find(topicInfos.begin(), topicInfos.end(), "dmo:a:3"), topicInfos.end());
        EXPECT_NE(std::find(topics.begin(), topics.end(), "the ring"), topics.end());
    }

    TEST_F(dmoDialogueOverlay, LuaBindingsQueueMutationsAndRefuseAtOnce)
    {
        std::vector<std::function<void()>> actions;
        LuaUtil::LuaState lua{ nullptr, nullptr };
        std::string error;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["records"] = MWLua::initDialogueRecordsBindings(
                view.sol(), dialogues(), [&](std::function<void()> action) { actions.push_back(std::move(action)); });
            auto result = view.sol().safe_script(R"(
                assert(records.createDialogue({type = 'topic', id = 'the ring', name = 'The Ring'}) == 'the ring')
                assert(records.insertInfo('the ring', {id = 'dmo:a:1', text = 'One', gender = 'female',
                    conditions = {{type = 'journal', variable = 'ms_fargothring', operator = '>=', value = 10}}},
                    {at = 'end'}) == 'dmo:a:1')
                assert(records.insertInfo('the ring', {id = 'dmo:a:2', text = 'Two', faction = 'FFFF'},
                    {before = 'dmo:a:1'}) == 'dmo:a:2')
                for _, call in ipairs({
                    function() records.createDialogue({type = 'topic', id = 'The Ring', name = 'The Ring'}) end,
                    function() records.createDialogue({type = 'journal', id = 'quest', name = 'Quest'}) end,
                    function() records.createDialogue({type = 'voice', id = 'dmo:v', name = 'V'}) end,
                    function() records.insertInfo('the ring', {id = 'plain'}, {at = 'end'}) end,
                    function() records.insertInfo('the ring', {id = 'dmo:j:1'}, {at = 'end'}) end,
                    function() records.insertInfo('the ring', {id = 'dmo:a:1'}, {at = 'end'}) end,
                    function() records.insertInfo('the ring', {id = 'dmo:a:3'}, {after = 'dmo:a:9'}) end,
                    function() records.insertInfo('the ring', {id = 'dmo:a:3'}, {at = 'middle'}) end,
                    function() records.insertInfo('the ring', {id = 'dmo:a:3', mood = 'odd'}, {at = 'end'}) end,
                    function() records.insertInfo('background', {id = 'dmo:a:3'}, {at = 'end'}) end,
                    function() records.insertInfo('the ring', {id = 'dmo:a:3',
                        conditions = {{type = 'pc_level', variable = 'x', operator = '==', value = 1}}}, {at = 'end'}) end,
                }) do
                    assert(not pcall(call))
                end
                assert(#records.listDeltas() == 0)
            )", sol::script_pass_on_error);
            if (!result.valid())
                error = result.get<sol::error>().what();
        });
        ASSERT_TRUE(error.empty()) << error;
        EXPECT_EQ(dialogues().search(id("the ring")), nullptr);
        ASSERT_EQ(actions.size(), 3);
        for (const auto& action : actions)
            action();
        const ESM::Dialogue* ring = dialogues().search(id("the ring"));
        ASSERT_NE(ring, nullptr);
        EXPECT_EQ(ids(*ring), (std::vector<std::string>{ "dmo:a:2", "dmo:a:1" }));
        EXPECT_EQ(ring->mInfo.back().mData.mGender, ESM::DialInfo::Female);
        EXPECT_TRUE(ring->mInfo.front().mFactionLess);
        EXPECT_FALSE(ring->mInfo.back().mFactionLess);
        ASSERT_EQ(ring->mInfo.back().mSelects.size(), 1);
        EXPECT_EQ(ring->mInfo.back().mSelects[0].mComparison, ESM::DialogueCondition::Comp_Ge);

        lua.protectedCall([&](LuaUtil::LuaView& view) {
            auto result = view.sol().safe_script(R"(
                local deltas = records.listDeltas()
                assert(#deltas == 3)
                assert(deltas[1].kind == 'dialogue' and deltas[1].dialogue == 'the ring' and deltas[1].op == 'created')
                assert(deltas[2].kind == 'info' and deltas[2].info == 'dmo:a:1' and deltas[2].conflict == nil)
            )", sol::script_pass_on_error);
            if (!result.valid())
                error = result.get<sol::error>().what();
        });
        EXPECT_TRUE(error.empty()) << error;
    }

    TEST_F(dmoDialogueOverlay, MutationsQueuedBeforeALoadAreDropped)
    {
        std::vector<std::function<void()>> actions;
        LuaUtil::LuaState lua{ nullptr, nullptr };
        std::string error;
        const auto run = [&](std::string_view script) {
            lua.protectedCall([&](LuaUtil::LuaView& view) {
                auto result = view.sol().safe_script(script, sol::script_pass_on_error);
                if (!result.valid())
                    error = result.get<sol::error>().what();
            });
        };
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["records"] = MWLua::initDialogueRecordsBindings(
                view.sol(), dialogues(), [&](std::function<void()> action) { actions.push_back(std::move(action)); });
        });
        run(R"(
            records.createDialogue({type = 'topic', id = 'the ring', name = 'The Ring'})
            records.insertInfo('the ring', {id = 'dmo:a:1'}, {at = 'end'})
        )");
        ASSERT_TRUE(error.empty()) << error;
        dialogues().clearDynamic();
        for (const auto& action : actions)
            action();
        actions.clear();
        EXPECT_EQ(dialogues().search(id("the ring")), nullptr);

        // What was pending belongs to the game before the load: the same calls are taken afresh.
        run(R"(
            records.createDialogue({type = 'topic', id = 'the ring', name = 'The Ring'})
            records.insertInfo('the ring', {id = 'dmo:a:1'}, {at = 'end'})
        )");
        ASSERT_TRUE(error.empty()) << error;
        for (const auto& action : actions)
            action();
        const ESM::Dialogue* ring = dialogues().search(id("the ring"));
        ASSERT_NE(ring, nullptr);
        EXPECT_EQ(ids(*ring), (std::vector<std::string>{ "dmo:a:1" }));
    }
}
