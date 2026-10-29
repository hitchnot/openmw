#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwclass/container.hpp>
#include <apps/openmw/mwclass/npc.hpp>
#include <apps/openmw/mwgui/tradeitemmodel.hpp>
#include <apps/openmw/mwlua/context.hpp>
#include <apps/openmw/mwlua/enginehooks.hpp>
#include <apps/openmw/mwlua/luamanagerimp.hpp>
#include <apps/openmw/mwlua/objectbindings.hpp>
#include <apps/openmw/mwlua/playerscripts.hpp>
#include <apps/openmw/mwworld/esmstore.hpp>
#include <apps/openmw/mwworld/livecellref.hpp>
#include <apps/openmw/mwworld/worldimp.hpp>
#include <apps/openmw/mwworld/worldmodel.hpp>

#include <components/esm/luascripts.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/lua/configuration.hpp>
#include <components/settings/values.hpp>
#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    TEST(dmoEngineHooks, AdmitsSixtyFourAFrameAndCountsTheDroppedExcess)
    {
        MWLua::EngineHookGate gate;
        for (int i = 0; i < MWLua::EngineHookGate::sPerFrame; ++i)
            ASSERT_TRUE(gate.admit()) << i;
        EXPECT_FALSE(gate.admit());
        EXPECT_FALSE(gate.admit());
        EXPECT_EQ(gate.dropped(), 2u);

        gate.newFrame();
        EXPECT_TRUE(gate.admit());
        EXPECT_EQ(gate.dropped(), 2u);
        EXPECT_EQ(MWLua::EngineHookGate::sPerFrame, 64);
    }
}

namespace MWLua
{
    class EngineHooksTest : public testing::Test
    {
    protected:
        MWBase::Environment mEnvironment;
        MWWorld::World mWorld{ nullptr, -1, {}, {} };
        TestingOpenMW::VFSTestFile mScript{ R"(
            local received = require('test.observer').received
            local handlers = {}
            for _, name in ipairs({'LockPicked', 'LockFailed', 'SpellCast', 'LevelUp', 'RestStarted',
                                  'BarterCompleted', 'ServiceUsed', 'DialogueTopicSelected'}) do
                handlers[name] = function(data)
                    received[#received + 1] = {name = name, data = data}
                end
            end
            return {eventHandlers = handlers}
        )" };
        std::unique_ptr<VFS::Manager> mVfs
            = TestingOpenMW::createTestVFS({ { VFS::Path::NormalizedView("hooks.lua"), &mScript } });
        std::unique_ptr<LuaManager> mManager;
        std::unique_ptr<MWWorld::LiveCellRef<ESM::NPC>> mPlayerRef;
        std::unique_ptr<MWWorld::LiveCellRef<ESM::NPC>> mActorRef;
        std::unique_ptr<MWWorld::LiveCellRef<ESM::Container>> mLockRef;
        MWWorld::Ptr mPlayer;
        MWWorld::Ptr mActor;
        MWWorld::Ptr mLock;
        const ESM::RefId mSpell = ESM::RefId::stringRefId("test_spell");
        const bool mOriginalAllActors = Settings::lua().mDmoHooksSpellCastAllActors;

        void SetUp() override
        {
            mEnvironment.setWorld(mWorld);
            mEnvironment.setESMStore(mWorld.getStore());
            mEnvironment.setWorldModel(mWorld.getWorldModel());
            MWClass::Npc::registerSelf();
            MWClass::Container::registerSelf();
            mPlayerRef = makeObject<ESM::NPC>("player");
            mActorRef = makeObject<ESM::NPC>("test_actor");
            mLockRef = makeObject<ESM::Container>("test_lock");
            mPlayer = MWWorld::Ptr(mPlayerRef.get());
            mActor = MWWorld::Ptr(mActorRef.get());
            mLock = MWWorld::Ptr(mLockRef.get());
        }

        void TearDown() override
        {
            mPlayer.getRefData().setLuaScripts(nullptr);
            mManager.reset();
            Settings::lua().mDmoHooksSpellCastAllActors.set(mOriginalAllActors);
        }

        template <class T>
        std::unique_ptr<MWWorld::LiveCellRef<T>> makeObject(std::string_view name)
        {
            T record;
            record.blank();
            record.mId = ESM::RefId::stringRefId(name);
            mWorld.getStore().insertStatic(record);
            ESM::CellRef ref;
            ref.blank();
            ref.mRefID = record.mId;
            auto result = std::make_unique<MWWorld::LiveCellRef<T>>(ref, mWorld.getStore().get<T>().find(record.mId));
            mWorld.getWorldModel().registerPtr(MWWorld::Ptr(result.get()));
            return result;
        }

        void start(bool allActors = false)
        {
            Settings::lua().mDmoHooksSpellCastAllActors.set(allActors);
            mManager = std::make_unique<LuaManager>(mVfs.get(), std::filesystem::path{});
            mEnvironment.setLuaManager(*mManager);

            // Bootstrap only the player consumer, without a renderer or GUI. Notifications, serialization,
            // the event queue and Lua event handlers below are the production delivery path.
            ESM::LuaScriptsCfg config;
            LuaUtil::parseOMWScripts(config, "CUSTOM: hooks.lua\n");
            mManager->mConfiguration.init(std::move(config), false);
            mManager->mPlayer = mPlayer;
            Context context;
            context.mType = Context::Local;
            context.mLua = &mManager->mLua;
            context.mLuaManager = mManager.get();
            context.mSerializer = mManager->mLocalSerializer.get();
            context.mLuaEvents = &mManager->mLuaEvents;
            initObjectBindingsForLocalScripts(context);
            sol::state_view lua = mManager->mLua.unsafeState();
            sol::table received = lua.create_table();
            lua["received"] = received;
            lua["player"] = LObject(mPlayer);
            lua["actor"] = LObject(mActor);
            lua["lock"] = LObject(mLock);
            auto scripts = std::make_shared<PlayerScripts>(&mManager->mLua, LObject(mPlayer));
            scripts->setSerializer(mManager->mLocalSerializer.get());
            scripts->addPackage("test.observer", LuaUtil::makeReadOnly(lua.create_table_with("received", received)));
            const auto scriptId = mManager->mConfiguration.findId(VFS::Path::NormalizedView("hooks.lua"));
            ASSERT_TRUE(scriptId.has_value());
            ASSERT_TRUE(scripts->addCustomScript(*scriptId));
            mPlayer.getRefData().setLuaScripts(std::move(scripts));
        }

        void deliver()
        {
            mManager->mLuaEvents.finalizeEventBatch();
            mManager->mLuaEvents.callEventHandlers();
        }

        void check(const std::string& code)
        {
            sol::state_view lua = mManager->mLua.unsafeState();
            const auto result = lua.safe_script(code, sol::script_pass_on_error);
            ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
        }
    };

    using dmoEngineHookEvents = EngineHooksTest;

    TEST_F(dmoEngineHookEvents, DialogueTopicSelectionReportsActorIdAndPseudoFlag)
    {
        start();
        mManager->onDialogueTopicSelected(mActor, "Door", true);
        mManager->onDialogueTopicSelected(mActor, "Background");
        deliver();
        check(R"(
            assert(#received == 2)
            assert(received[1].name == 'DialogueTopicSelected')
            assert(received[1].data.actor == actor and received[1].data.topic == 'door')
            assert(received[1].data.pseudo == true)
            assert(received[2].data.actor == actor and received[2].data.topic == 'background')
            assert(received[2].data.pseudo == false)
        )");
    }

    TEST_F(dmoEngineHookEvents, LockPickedReportsAttemptedObject)
    {
        start();
        mManager->onLockPicked(mLock);
        check("assert(#received == 0)");
        deliver();
        check(R"(
            assert(#received == 1 and received[1].name == 'LockPicked')
            assert(received[1].data.object == lock)
            assert(received[1].data.object.recordId == 'test_lock')
        )");
    }

    TEST_F(dmoEngineHookEvents, LockFailedReportsAttemptedObject)
    {
        start();
        mManager->onLockFailed(mLock);
        deliver();
        check(R"(
            assert(#received == 1 and received[1].name == 'LockFailed')
            assert(received[1].data.object == lock)
            assert(received[1].data.object.recordId == 'test_lock')
        )");
    }

    TEST_F(dmoEngineHookEvents, SpellCastReportsCasterSpellAndAvailableTarget)
    {
        start();
        mManager->onSpellCast(mPlayer, mSpell, mActor);
        mManager->onSpellCast(mPlayer, mSpell, {});
        deliver();
        check(R"(
            assert(#received == 2)
            for _, event in ipairs(received) do
                assert(event.name == 'SpellCast')
                assert(event.data.caster == player and event.data.spell == 'test_spell')
            end
            assert(received[1].data.target == actor)
            assert(received[2].data.target == nil)
        )");
    }

    TEST_F(dmoEngineHookEvents, SpellCastIncludesOtherActorsOnlyWhenEnabledAtStartup)
    {
        start();
        mManager->onSpellCast(mActor, mSpell, mPlayer);
        mManager->onSpellCast(mPlayer, mSpell, {});
        deliver();
        check("assert(#received == 1 and received[1].data.caster == player)");

        mPlayer.getRefData().setLuaScripts(nullptr);
        mManager.reset();
        start(true);
        Settings::lua().mDmoHooksSpellCastAllActors.set(false);
        mManager->onSpellCast(mActor, mSpell, mPlayer);
        mManager->onSpellCast(mPlayer, mSpell, {});
        mManager->onSpellCast(mLock, mSpell, mPlayer);
        deliver();
        check(R"(
            assert(#received == 2)
            assert(received[1].name == 'SpellCast' and received[1].data.caster == actor)
            assert(received[1].data.spell == 'test_spell' and received[1].data.target == player)
            assert(received[2].name == 'SpellCast' and received[2].data.caster == player)
        )");
    }

    TEST_F(dmoEngineHookEvents, LevelUpReportsCommittedLevel)
    {
        start();
        mManager->onLevelUp(7);
        deliver();
        check(R"(
            assert(#received == 1 and received[1].name == 'LevelUp')
            assert(received[1].data.level == 7)
        )");
    }

    TEST_F(dmoEngineHookEvents, BarterCompletedReportsMerchantRecordCountsAndSignedGold)
    {
        start();
        const std::map<ESM::RefId, int> bought{ { ESM::RefId::stringRefId("iron_arrow"), 20 },
            { ESM::RefId::stringRefId("restore_health"), 3 } };
        const std::map<ESM::RefId, int> sold{ { ESM::RefId::stringRefId("iron_dagger"), 2 } };
        mManager->onBarterCompleted(mActor, bought, sold, -35);
        mManager->onBarterCompleted(mActor, {}, sold, 12);
        mManager->onBarterCompleted(mActor, bought, {}, 0);
        check("assert(#received == 0)");
        deliver();
        check(R"(
            assert(#received == 3)
            for _, event in ipairs(received) do
                assert(event.name == 'BarterCompleted')
                assert(event.data.actor == actor and event.data.actor.recordId == 'test_actor')
            end
            local data = received[1].data
            assert(#data.bought == 2 and #data.sold == 1 and data.gold == -35)
            local bought = {}
            for _, item in ipairs(data.bought) do bought[item.record] = item.count end
            assert(bought.iron_arrow == 20 and bought.restore_health == 3)
            assert(data.sold[1].record == 'iron_dagger' and data.sold[1].count == 2)
            assert(#received[2].data.bought == 0 and received[2].data.gold == 12)
            assert(#received[3].data.sold == 0 and received[3].data.gold == 0)
        )");
    }

    TEST_F(dmoEngineHookEvents, CancelledBarterProducesNoCompletion)
    {
        start();
        // Exercise the same abort operations as TradeWindow::exit without constructing GUI widgets.
        class OfferItems : public MWGui::ProxyItemModel
        {
        public:
            MWGui::ItemStack mItem;
            MWGui::ItemStack getItem(ModelIndex) override { return mItem; }
            size_t getItemCount() override { return 1; }
            void update() override {}
        };
        OfferItems source;
        source.mItem.mBase = mLock;
        source.mItem.mCount = 3;
        MWGui::TradeItemModel playerItems(nullptr, {});
        MWGui::TradeItemModel merchantItems(nullptr, mActor);
        playerItems.borrowItemToUs(0, &source, 2);
        merchantItems.borrowItemToUs(0, &source, 1);
        merchantItems.abort();
        playerItems.abort();
        deliver();
        check("assert(#received == 0)");

        // A subsequent completed transaction is still delivered to the same Lua consumer.
        mManager->onBarterCompleted(mActor, { { ESM::RefId::stringRefId("iron_arrow"), 5 } }, {}, -10);
        deliver();
        check(R"(
            assert(#received == 1 and received[1].name == 'BarterCompleted')
            local data = received[1].data
            assert(data.actor == actor and data.gold == -10)
            assert(#data.bought == 1 and data.bought[1].record == 'iron_arrow' and data.bought[1].count == 5)
            assert(#data.sold == 0)
        )");
    }

    TEST_F(dmoEngineHookEvents, ServiceUsedReportsServiceActorAndOnlySuppliedDetail)
    {
        start();
        const std::string createdSpell = ESM::RefId::generated(42).serializeText();
        check("createdSpell = '" + createdSpell + "'");
        mManager->onServiceUsed("training", mActor, { { "skill", std::string("alchemy") }, { "price", 50 } });
        mManager->onServiceUsed("spells", mActor, { { "spell", mSpell.serializeText() }, { "price", 20 } });
        mManager->onServiceUsed("spellmaking", mActor, { { "spell", createdSpell }, { "price", 100 } });
        mManager->onServiceUsed("enchanting", mActor,
            { { "record", std::string("enchanted_arrow") }, { "count", 10 },
                { "enchantment", std::string("test_enchantment") } });
        mManager->onServiceUsed("repair", mActor, { { "record", std::string("iron_dagger") }, { "price", 4 } });
        mManager->onServiceUsed("training", mActor);
        check("assert(#received == 0)");
        deliver();
        check(R"(
            local services = {'training', 'spells', 'spellmaking', 'enchanting', 'repair', 'training'}
            assert(#received == #services)
            for i, event in ipairs(received) do
                assert(event.name == 'ServiceUsed' and event.data.service == services[i])
                assert(event.data.actor == actor and event.data.actor.recordId == 'test_actor')
            end
            assert(received[1].data.detail.skill == 'alchemy' and received[1].data.detail.price == 50)
            assert(received[2].data.detail.spell == 'test_spell' and received[2].data.detail.price == 20)
            assert(received[3].data.detail.spell == createdSpell and received[3].data.detail.price == 100)
            local enchanted = received[4].data.detail
            assert(enchanted.record == 'enchanted_arrow' and enchanted.count == 10)
            assert(enchanted.enchantment == 'test_enchantment' and enchanted.price == nil)
            assert(received[5].data.detail.record == 'iron_dagger' and received[5].data.detail.price == 4)
            assert(received[6].data.detail == nil)
        )");
    }

    TEST_F(dmoEngineHookEvents, NewHooksShareTheExistingFrameCap)
    {
        start();
        for (int i = 0; i < 58; ++i)
            mManager->onRestStarted(1);
        mManager->onLockPicked(mLock);
        mManager->onLockFailed(mLock);
        mManager->onSpellCast(mPlayer, mSpell, {});
        mManager->onLevelUp(7);
        mManager->onBarterCompleted(mActor, {}, {}, 0);
        mManager->onServiceUsed("repair", mActor);
        mManager->onLockPicked(mLock);
        mManager->onLockFailed(mLock);
        mManager->onSpellCast(mPlayer, mSpell, {});
        mManager->onLevelUp(8);
        mManager->onBarterCompleted(mActor, {}, {}, 1);
        mManager->onServiceUsed("spells", mActor);
        deliver();
        check(R"(
            assert(#received == 64)
            assert(received[59].name == 'LockPicked' and received[60].name == 'LockFailed')
            assert(received[61].name == 'SpellCast' and received[62].name == 'LevelUp')
            assert(received[62].data.level == 7)
            assert(received[63].name == 'BarterCompleted' and received[63].data.gold == 0)
            assert(received[64].name == 'ServiceUsed' and received[64].data.service == 'repair')
        )");
    }
}
