#include <apps/openmw/mwdialogue/dialoguechoices.hpp>
#include <apps/openmw/mwdialogue/dialoguemanagerimp.hpp>
#include <apps/openmw/mwlua/corebindings.hpp>

#include <components/compiler/extensions.hpp>
#include <components/lua/luastate.hpp>
#include <components/translation/translation.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

namespace
{
    class ChoiceManager : public MWDialogue::DialogueManager
    {
    public:
        using MWDialogue::DialogueManager::DialogueManager;
        std::vector<MWDialogue::ChoiceSelection> mEvents;
        std::vector<std::string> mOrder;
        std::vector<int> mVanillaAnswers;
        bool mChoicesClearedAtEvent = false;

    protected:
        void answerVanilla(int answer, ResponseCallback*) override
        {
            mVanillaAnswers.push_back(answer);
            mOrder.emplace_back("response");
        }

        void emitChoiceSelected(const MWDialogue::ChoiceSelection& selection) override
        {
            mEvents.push_back(selection);
            mOrder.emplace_back("event");
            mChoicesClearedAtEvent = getChoices().empty();
        }
    };

    class dmoDialogueChoiceRange : public testing::Test
    {
    protected:
        Compiler::Extensions mExtensions;
        Translation::Storage mTranslation;
        ChoiceManager mManager{ mExtensions, mTranslation };
    };

    TEST_F(dmoDialogueChoiceRange, ReservedRangeIsHalfOpen)
    {
        EXPECT_FALSE(MWDialogue::isLuaChoiceId(std::numeric_limits<int>::min()));
        EXPECT_FALSE(MWDialogue::isLuaChoiceId(-1));
        EXPECT_FALSE(MWDialogue::isLuaChoiceId(0x3ffffff));
        EXPECT_TRUE(MWDialogue::isLuaChoiceId(0x4000000));
        EXPECT_TRUE(MWDialogue::isLuaChoiceId(0x7ffffffe));
        EXPECT_FALSE(MWDialogue::isLuaChoiceId(0x7fffffff));
    }

    TEST_F(dmoDialogueChoiceRange, RealDispatcherClearsAndMapsLuaSelectionWithoutRunningVanilla)
    {
        mManager.addLuaChoices("invitation", { { "accept", "Accept" }, { "decline", "Decline" } }, true);
        const auto choices = mManager.getChoices();
        ASSERT_EQ(choices.size(), 2);
        EXPECT_EQ(choices[0].first, "Accept");
        EXPECT_EQ(choices[1].first, "Decline");
        EXPECT_TRUE(MWDialogue::isLuaChoiceId(choices[0].second));
        EXPECT_NE(choices[0].second, choices[1].second);
        mManager.addLuaChoices("other", { { "wait", "Wait" } }, false);
        mManager.questionAnswered(choices[1].second, nullptr);
        EXPECT_TRUE(mManager.mVanillaAnswers.empty());
        ASSERT_EQ(mManager.mEvents.size(), 1);
        EXPECT_EQ(mManager.mEvents[0].mOfferId, "invitation");
        EXPECT_EQ(mManager.mEvents[0].mChoiceId, "decline");
        EXPECT_TRUE(mManager.mEvents[0].mLua);
        EXPECT_TRUE(mManager.mChoicesClearedAtEvent);
        EXPECT_FALSE(mManager.isInChoice());
        mManager.questionAnswered(choices[1].second, nullptr);
        EXPECT_EQ(mManager.mEvents.size(), 1);
        EXPECT_TRUE(mManager.mVanillaAnswers.empty());
    }

    TEST_F(dmoDialogueChoiceRange, UnmappedReservedIdsNeverReachVanillaOrClearAnOffer)
    {
        mManager.addLuaChoices("pending", { { "a", "A" } }, true);
        const auto choices = mManager.getChoices();
        mManager.questionAnswered(0x7ffffffe, nullptr);
        EXPECT_TRUE(mManager.mVanillaAnswers.empty());
        EXPECT_TRUE(mManager.mEvents.empty());
        EXPECT_EQ(mManager.getChoices(), choices);
    }

    TEST_F(dmoDialogueChoiceRange, NonReservedAnswersUseVanillaBeforeReportingSelection)
    {
        for (int id : { -1, 0, 42, 0x3ffffff, 0x7fffffff })
        {
            mManager.addLuaChoices("pending", { { "a", "A" } }, true);
            mManager.questionAnswered(id, nullptr);
            ASSERT_FALSE(mManager.mEvents.empty());
            EXPECT_EQ(mManager.mVanillaAnswers.back(), id);
            EXPECT_EQ(mManager.mEvents.back().mOfferId, "");
            EXPECT_EQ(mManager.mEvents.back().mChoiceId, std::to_string(id));
            EXPECT_FALSE(mManager.mEvents.back().mLua);
            EXPECT_TRUE(mManager.mChoicesClearedAtEvent);
            EXPECT_EQ(mManager.mOrder[mManager.mOrder.size() - 2], "response");
            EXPECT_EQ(mManager.mOrder.back(), "event");
        }
    }

    TEST_F(dmoDialogueChoiceRange, ExclusivityTracksEveryOfferAndPreservesVanillaLock)
    {
        mManager.addLuaChoices("ambient", { { "a", "A" } }, false);
        EXPECT_FALSE(mManager.isInChoice());
        mManager.addLuaChoices("question", { { "b", "B" } }, true);
        mManager.addLuaChoices("second", { { "c", "C" } }, true);
        EXPECT_TRUE(mManager.isInChoice());
        mManager.clearLuaChoices("question");
        EXPECT_TRUE(mManager.isInChoice());
        mManager.clearLuaChoices("second");
        EXPECT_FALSE(mManager.isInChoice());
        mManager.addChoice("Vanilla", 7);
        mManager.clearLuaChoices();
        EXPECT_TRUE(mManager.isInChoice());
        ASSERT_EQ(mManager.getChoices().size(), 1);
        EXPECT_EQ(mManager.getChoices()[0].second, 7);
    }

    TEST_F(dmoDialogueChoiceRange, LuaSelectionPreservesPendingVanillaChoices)
    {
        mManager.addChoice("Vanilla", 4);
        mManager.addLuaChoices("offer", { { "a", "A" } }, true);
        const int luaId = mManager.getChoices().back().second;
        mManager.questionAnswered(luaId, nullptr);
        ASSERT_EQ(mManager.getChoices().size(), 1);
        EXPECT_EQ(mManager.getChoices()[0].second, 4);
        EXPECT_TRUE(mManager.isInChoice());
        EXPECT_TRUE(mManager.mVanillaAnswers.empty());
        ASSERT_EQ(mManager.mEvents.size(), 1);
        EXPECT_TRUE(mManager.mEvents[0].mLua);
    }

    TEST_F(dmoDialogueChoiceRange, ReplacingOfferUpdatesLockAndDoesNotReuseIds)
    {
        mManager.addLuaChoices("offer", { { "a", "Old" } }, true);
        const int oldId = mManager.getChoices()[0].second;
        mManager.addLuaChoices("offer", { { "a", "New" } }, false);
        ASSERT_EQ(mManager.getChoices().size(), 1);
        EXPECT_EQ(mManager.getChoices()[0].first, "New");
        EXPECT_GT(mManager.getChoices()[0].second, oldId);
        EXPECT_FALSE(mManager.isInChoice());
        mManager.questionAnswered(oldId, nullptr);
        EXPECT_TRUE(mManager.mEvents.empty());
        EXPECT_EQ(mManager.getChoices().size(), 1);
        mManager.addLuaChoices("offer", {}, true);
        EXPECT_TRUE(mManager.getChoices().empty());
        EXPECT_FALSE(mManager.isInChoice());
    }

    TEST_F(dmoDialogueChoiceRange, ClearingUnknownOfferLeavesChoicesUntouched)
    {
        mManager.addLuaChoices("offer", { { "a", "A" } }, true);
        const auto choices = mManager.getChoices();
        mManager.clearLuaChoices("missing");
        EXPECT_EQ(mManager.getChoices(), choices);
        EXPECT_TRUE(mManager.isInChoice());
    }

    TEST_F(dmoDialogueChoiceRange, GameClearInvalidatesSessionAndMapping)
    {
        mManager.addLuaChoices("offer", { { "a", "A" } }, true);
        const int oldId = mManager.getChoices()[0].second;
        const auto session = mManager.dialogueSession();
        mManager.clear();
        EXPECT_NE(mManager.dialogueSession(), session);
        EXPECT_TRUE(mManager.getChoices().empty());
        EXPECT_FALSE(mManager.isInChoice());
        mManager.addLuaChoices("offer", { { "a", "A" } }, true);
        EXPECT_GT(mManager.getChoices()[0].second, oldId);
        mManager.questionAnswered(oldId, nullptr);
        EXPECT_TRUE(mManager.mEvents.empty());
        EXPECT_TRUE(mManager.mVanillaAnswers.empty());
    }

    TEST_F(dmoDialogueChoiceRange, GoodbyeDropsLuaOffers)
    {
        mManager.addLuaChoices("offer", { { "a", "A" } }, true);
        mManager.goodbye();
        EXPECT_TRUE(mManager.isGoodbye());
        EXPECT_FALSE(mManager.isInChoice());
        EXPECT_TRUE(mManager.getChoices().empty());
    }

    TEST_F(dmoDialogueChoiceRange, EndingConversationInvalidatesSessionAndChoices)
    {
        mManager.addLuaChoices("offer", { { "a", "A" } }, true);
        const int oldId = mManager.getChoices()[0].second;
        const auto session = mManager.dialogueSession();
        mManager.goodbyeSelected();
        EXPECT_NE(mManager.dialogueSession(), session);
        EXPECT_TRUE(mManager.getChoices().empty());
        EXPECT_FALSE(mManager.isInChoice());
        mManager.questionAnswered(oldId, nullptr);
        EXPECT_TRUE(mManager.mEvents.empty());
        EXPECT_TRUE(mManager.mVanillaAnswers.empty());
    }

    TEST(dmoDialogueBindings, ApiExtensionsAdvertisesDialogueAndRejectsWrites)
    {
        LuaUtil::LuaState lua{ nullptr, nullptr };
        std::string error;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["extensions"] = MWLua::initCoreApiExtensions(view.sol());
            auto result = view.sol().safe_script(R"(
                assert(extensions.dialogue == 1 and extensions.watch == 1 and extensions.ipc == 1)
                assert(not pcall(function() extensions.dialogue = 2 end))
                assert(not pcall(function() extensions.other = 1 end))
            )", sol::script_pass_on_error);
            if (!result.valid())
                error = result.get<sol::error>().what();
        });
        ASSERT_TRUE(error.empty()) << error;
    }
}
