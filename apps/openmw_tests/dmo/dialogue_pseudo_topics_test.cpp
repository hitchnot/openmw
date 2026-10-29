#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwclass/npc.hpp>
#include <apps/openmw/mwdialogue/dialoguemanagerimp.hpp>
#include <apps/openmw/mwlua/dialoguebindings_ext.hpp>
#include <apps/openmw/mwlua/object.hpp>
#include <apps/openmw/mwworld/esmstore.hpp>
#include <apps/openmw/mwworld/livecellref.hpp>
#include <apps/openmw/mwworld/worldimp.hpp>
#include <apps/openmw/mwworld/worldmodel.hpp>

#include <components/compiler/extensions.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/formatversion.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/translation/translation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    class TopicManager : public MWDialogue::DialogueManager
    {
    public:
        using MWDialogue::DialogueManager::DialogueManager;
        std::vector<std::pair<MWWorld::Ptr, std::string>> mSelections;
        int mVanillaResponses = 0;

        void keywordSelected(std::string_view, ResponseCallback*) override { ++mVanillaResponses; }

    protected:
        void emitPseudoTopicSelected(const MWWorld::Ptr& actor, const std::string& topicId) override
        {
            mSelections.emplace_back(actor, topicId);
        }
    };

    class dmoDialoguePseudoTopics : public testing::Test
    {
    protected:
        MWBase::Environment mEnvironment;
        MWWorld::World mWorld{ nullptr, -1, {}, {} };
        Compiler::Extensions mExtensions;
        Translation::Storage mTranslation;
        TopicManager mDialogue{ mExtensions, mTranslation };
        LuaUtil::LuaState mLua{ nullptr, nullptr };
        std::unique_ptr<MWWorld::LiveCellRef<ESM::NPC>> mFirstRef;
        std::unique_ptr<MWWorld::LiveCellRef<ESM::NPC>> mSecondRef;
        MWWorld::Ptr mFirst;
        MWWorld::Ptr mSecond;
        MWWorld::Ptr mOpenActor;
        std::vector<std::function<void()>> mActions;
        std::vector<MWDialogue::PseudoTopic> mVisible;
        int mRefreshes = 0;

        void SetUp() override
        {
            mEnvironment.setWorld(mWorld);
            mEnvironment.setESMStore(mWorld.getStore());
            mEnvironment.setWorldModel(mWorld.getWorldModel());
            mEnvironment.setDialogueManager(mDialogue);
            MWClass::Npc::registerSelf();
            ESM::NPC record;
            record.blank();
            record.mId = ESM::RefId::stringRefId("same_actor_record");
            mWorld.getStore().insertStatic(record);
            // Two instances of one base record must not share pseudo-topics.
            mFirstRef = makeActor(record.mId);
            mSecondRef = makeActor(record.mId);
            mFirst = MWWorld::Ptr(mFirstRef.get());
            mSecond = MWWorld::Ptr(mSecondRef.get());
            mOpenActor = mFirst;

            const auto path
                = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files/data/scripts/omw/dialogue.lua";
            std::ifstream stream(path);
            ASSERT_TRUE(stream.is_open());
            const std::string source{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
            mLua.protectedCall([&](LuaUtil::LuaView& view) {
                sol::state_view lua = view.sol();
                lua.new_usertype<MWLua::LObject>("GameObject", sol::no_constructor);
                lua["first"] = MWLua::LObject(mFirst);
                lua["second"] = MWLua::LObject(mSecond);
                sol::table api = lua.create_table();
                // Only the rendered window is substituted. The shipped interface script,
                // bindings, queued actions and actor-specific dispatcher are production code.
                MWLua::addDialogueTopicBindings(api, mDialogue,
                    [this](std::function<void()> action) { mActions.push_back(std::move(action)); },
                    { [this] { return mOpenActor; }, [this](std::string_view id) { return canSelect(id); },
                        [this](const std::string& id) {
                            if (!canSelect(id))
                                return;
                            if (Misc::StringUtils::ciEqual(id, "ordinary"))
                                mDialogue.keywordSelected(id, nullptr);
                            else
                                mDialogue.selectPseudoTopic(mOpenActor, id);
                        },
                        [this] { refresh(); } });
                sol::table interfaces = lua.create_table();
                sol::table ui = lua.create_table_with("dialogue", LuaUtil::makeReadOnly(api));
                lua["require"]
                    = [interfaces, ui](const std::string& name) { return name == "openmw.ui" ? ui : interfaces; };
                const auto result = lua.safe_script(source, sol::script_pass_on_error);
                ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
                sol::table script = result;
                interfaces["Dialogue"] = script["interface"];
                lua["I"] = interfaces;
            });
        }

        std::unique_ptr<MWWorld::LiveCellRef<ESM::NPC>> makeActor(const ESM::RefId& id)
        {
            ESM::CellRef ref;
            ref.blank();
            ref.mRefID = id;
            auto result
                = std::make_unique<MWWorld::LiveCellRef<ESM::NPC>>(ref, mWorld.getStore().get<ESM::NPC>().find(id));
            mWorld.getWorldModel().registerPtr(MWWorld::Ptr(result.get()));
            return result;
        }

        bool canSelect(std::string_view id) const
        {
            if (mOpenActor.isEmpty() || mDialogue.isInChoice() || mDialogue.isGoodbye())
                return false;
            return Misc::StringUtils::ciEqual(id, "ordinary")
                || std::any_of(mVisible.begin(), mVisible.end(),
                    [&](const auto& topic) { return Misc::StringUtils::ciEqual(id, topic.mId); });
        }

        void refresh()
        {
            ++mRefreshes;
            mVisible = mDialogue.getPseudoTopics(mOpenActor);
        }

        void open(const MWWorld::Ptr& actor)
        {
            mDialogue.goodbyeSelected();
            mOpenActor = actor;
            refresh();
        }

        void run(std::string_view code)
        {
            mLua.protectedCall([&](LuaUtil::LuaView& view) {
                const auto result = view.sol().safe_script(code, sol::script_pass_on_error);
                ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
            });
        }

        void drain()
        {
            const auto actions = std::exchange(mActions, {});
            for (const auto& action : actions)
                action();
        }

        std::string save()
        {
            std::ostringstream stream;
            ESM::ESMWriter writer;
            writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
            writer.save(stream);
            Loading::Listener progress;
            mDialogue.write(writer, progress);
            writer.close();
            return stream.str();
        }
    };

    TEST_F(dmoDialoguePseudoTopics, PublicInterfaceIsolatesActorsAndSelectsAndRemovesById)
    {
        run(R"(
            I.Dialogue.addPseudoTopic(first, 'Door', 'Open a door')
            I.Dialogue.addPseudoTopic(second, 'Other', 'Open a door')
            assert(not I.Dialogue.selectTopic('door'))
        )");
        EXPECT_TRUE(mVisible.empty());
        drain();
        ASSERT_EQ(mVisible.size(), 1);
        EXPECT_EQ(mVisible[0].mLabel, "Open a door");
        run(R"(
            assert(not I.Dialogue.selectTopic('other'))
            assert(not I.Dialogue.selectTopic('Open a door'))
            assert(I.Dialogue.selectTopic('DOOR'))
            I.Dialogue.addPseudoTopic(first, 'door', 'Barter', {skin = 'MW_ListLine_Specific'})
        )");
        EXPECT_TRUE(mDialogue.mSelections.empty());
        drain();
        ASSERT_EQ(mDialogue.mSelections.size(), 1);
        EXPECT_EQ(mDialogue.mSelections[0], (std::pair{ mFirst, std::string("door") }));
        EXPECT_EQ(mDialogue.mVanillaResponses, 0);
        ASSERT_EQ(mVisible.size(), 1);
        EXPECT_EQ(mVisible[0].mLabel, "Barter");
        EXPECT_EQ(mVisible[0].mSkin, "MW_ListLine_Specific");
        open(mSecond);
        run(R"(
            assert(not I.Dialogue.selectTopic('door'))
            assert(I.Dialogue.selectTopic('other'))
            I.Dialogue.removePseudoTopic(first, 'DOOR')
        )");
        drain();
        ASSERT_EQ(mDialogue.mSelections.size(), 2);
        EXPECT_EQ(mDialogue.mSelections[1].first, mSecond);
        ASSERT_EQ(mVisible.size(), 1);
        EXPECT_EQ(mVisible[0].mId, "other");
        open(mFirst);
        EXPECT_TRUE(mVisible.empty());
        run("assert(not I.Dialogue.selectTopic('door')); assert(I.Dialogue.selectTopic('ordinary'))");
        drain();
        EXPECT_EQ(mDialogue.mVanillaResponses, 1);
        open(mSecond);
        run("I.Dialogue.removePseudoTopic(second, 'other'); assert(I.Dialogue.selectTopic('other'))");
        drain(); // Removal invalidates a selection already queued for this entry.
        EXPECT_TRUE(mVisible.empty());
        EXPECT_EQ(mDialogue.mSelections.size(), 2);
    }

    TEST_F(dmoDialoguePseudoTopics, RefreshAndEditsPreserveChoicesAndRespectSelectionGuards)
    {
        run("I.Dialogue.addPseudoTopic(first, 'door', 'Door')");
        drain();
        mDialogue.addLuaChoices("offer", { { "accept", "Accept" } }, true);
        const auto choices = mDialogue.getChoices();
        const int refreshes = mRefreshes;
        run(R"(
            local original = I.Dialogue
            I.Dialogue.addPseudoTopic(first, 'door', 'Updated')
            I.Dialogue.removePseudoTopic(first, 'absent')
            assert(I.Dialogue.refresh())
            assert(not I.Dialogue.selectTopic('door'))
            assert(original == I.Dialogue)
        )");
        EXPECT_EQ(mRefreshes, refreshes);
        drain();
        EXPECT_EQ(mRefreshes, refreshes + 3);
        EXPECT_EQ(mDialogue.getChoices(), choices);
        mDialogue.clearLuaChoices();
        run("assert(I.Dialogue.selectTopic('door'))");
        mDialogue.addChoice("Vanilla", 7);
        drain(); // A choice appearing before execution also blocks the selection.
        EXPECT_TRUE(mDialogue.mSelections.empty());
        const auto vanilla = mDialogue.getChoices();
        run("assert(I.Dialogue.refresh()); I.Dialogue.removePseudoTopic(first, 'door')");
        drain();
        EXPECT_EQ(mDialogue.getChoices(), vanilla);
        EXPECT_TRUE(mVisible.empty());
        mDialogue.goodbye();
        run("I.Dialogue.addPseudoTopic(first, 'door', 'Door')");
        drain();
        run("assert(not I.Dialogue.selectTopic('door'))");
        EXPECT_FALSE(mDialogue.selectPseudoTopic(mFirst, "door"));
        mOpenActor = {};
        run("assert(not I.Dialogue.refresh()); assert(not I.Dialogue.selectTopic('door'))");
    }

    TEST_F(dmoDialoguePseudoTopics, EntriesAreUnsavedAndClearDropsQueuedEditsFromThePreviousGame)
    {
        const std::string before = save();
        const auto records = mDialogue.countSavedGameRecords();
        mOpenActor = {};
        run("I.Dialogue.addPseudoTopic(first, 'door', 'Door')");
        drain();
        open(mFirst);
        ASSERT_EQ(mVisible.size(), 1);
        EXPECT_EQ(save(), before);
        EXPECT_EQ(mDialogue.countSavedGameRecords(), records);
        run("assert(I.Dialogue.selectTopic('door')); assert(I.Dialogue.refresh())");
        open(mSecond);
        drain();
        EXPECT_TRUE(mDialogue.mSelections.empty());
        open(mFirst);
        ASSERT_EQ(mVisible.size(), 1); // Closing a conversation preserves actor entries.
        run(R"(
            I.Dialogue.addPseudoTopic(second, 'stale', 'Stale')
            I.Dialogue.removePseudoTopic(first, 'door')
            assert(I.Dialogue.selectTopic('door'))
            assert(I.Dialogue.refresh())
        )");
        mDialogue.clear(); // Shared load/new-game path in StateManager::cleanup.
        run("I.Dialogue.addPseudoTopic(first, 'door', 'New game')");
        drain();
        EXPECT_TRUE(mDialogue.getPseudoTopics(mSecond).empty());
        ASSERT_EQ(mDialogue.getPseudoTopics(mFirst).size(), 1);
        EXPECT_EQ(mDialogue.getPseudoTopics(mFirst)[0].mLabel, "New game");
        EXPECT_TRUE(mDialogue.mSelections.empty());
        mDialogue.clear();
        EXPECT_TRUE(mDialogue.getPseudoTopics(mFirst).empty());
        EXPECT_EQ(save(), before);
    }
}
