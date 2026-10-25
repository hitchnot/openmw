#include <apps/openmw/mwlua/corebindings.hpp>
#include <apps/openmw/mwlua/object.hpp>
#include <apps/openmw/mwlua/userdataserializer.hpp>
#include <apps/openmw/mwlua/watch/watchbindings.hpp>
#include <apps/openmw/mwlua/watch/watchmanager.hpp>

#include <components/lua/luastate.hpp>
#include <components/lua/serialization.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace MWLua::Watch;
    using Kind = Predicate::Kind;
    const ESM::RefNum actor{ 1, 0 }, target{ 2, 0 };
    const ESM::RefId cell = ESM::RefId::stringRefId("hall");
    const ESM::RefId otherCell = ESM::RefId::stringRefId("vault");

    struct FixtureWorld final : World
    {
        std::optional<Location> mPlayer = Location{ {}, cell, cell };
        std::map<ESM::RefNum, Location> mObjects{
            { actor, { { 3, 4, 0 }, cell, cell } }, { target, { { 7, 0, 0 }, cell, cell } }
        };
        std::map<ESM::RefNum, CombatState> mStates{ { actor, CombatState::Calm }, { target, CombatState::Engaged } };
        bool mVisible = true;
        double mHour = 12;
        double mTime = 100;

        std::optional<Location> player() const override { return mPlayer; }
        std::optional<Location> object(ESM::RefNum ref) const override
        {
            auto it = mObjects.find(ref);
            return it == mObjects.end() ? std::nullopt : std::optional(it->second);
        }
        bool lineOfSight(ESM::RefNum from, ESM::RefNum to) const override
        {
            return mVisible && from == actor && to == target;
        }
        std::optional<CombatState> combatState(ESM::RefNum ref) const override
        {
            auto it = mStates.find(ref);
            return it == mStates.end() ? std::nullopt : std::optional(it->second);
        }
        double gameHour() const override { return mHour; }
        double gameTimeHours() const override { return mTime; }
    };

    Predicate nearActor(double radius = 5)
    {
        Predicate p;
        p.mRef = actor;
        p.mRadius = radius;
        return p;
    }

    class dmoWatchPredicate : public testing::Test
    {
    protected:
        FixtureWorld mWorld;
        Manager mManager;

        std::vector<Event> evaluate(Predicate p)
        {
            mManager.clear();
            mManager.registerWatch({ "probe", std::move(p), true });
            return mManager.update(mWorld);
        }
    };

    TEST_F(dmoWatchPredicate, NearUsesInclusiveThreeDimensionalRadiusAndWorldSpace)
    {
        const auto events = evaluate(nearActor());
        ASSERT_EQ(events.size(), 1);
        EXPECT_EQ(events[0].mId, "probe");
        EXPECT_FALSE(events[0].mExpired);
        ASSERT_EQ(events[0].mBindings.size(), 1);
        EXPECT_EQ(events[0].mBindings[0].mRef, actor);
        EXPECT_EQ(events[0].mBindings[0].mCell, cell);
        EXPECT_TRUE(evaluate(nearActor(4.99)).empty());
        mWorld.mObjects[actor].mPosition.z() = 1;
        EXPECT_TRUE(evaluate(nearActor()).empty());
        mWorld.mObjects[actor].mPosition.z() = 0;
        mWorld.mObjects[actor].mCell = otherCell;
        EXPECT_EQ(evaluate(nearActor()).size(), 1); // Adjacent exteriors can share a worldspace.
        mWorld.mObjects[actor].mWorldSpace = otherCell;
        EXPECT_TRUE(evaluate(nearActor()).empty());
    }

    TEST_F(dmoWatchPredicate, NearCellIgnoresRadiusAndRequiresPlayer)
    {
        Predicate p;
        p.mCell = cell;
        EXPECT_EQ(evaluate(p).size(), 1);
        p.mCell = otherCell;
        EXPECT_TRUE(evaluate(p).empty());
        mWorld.mPlayer.reset();
        EXPECT_TRUE(evaluate(nearActor()).empty());
        EXPECT_TRUE(evaluate(p).empty());
    }

    TEST_F(dmoWatchPredicate, LosUsesBothReferencesAndLiveVisibility)
    {
        Predicate p;
        p.mKind = Kind::Los;
        p.mRef = actor;
        p.mTo = target;
        const auto events = evaluate(p);
        ASSERT_EQ(events.size(), 1);
        EXPECT_EQ(events[0].mBindings[0].mFrom, actor);
        EXPECT_EQ(events[0].mBindings[0].mTo, target);
        mWorld.mVisible = false;
        EXPECT_TRUE(evaluate(p).empty());
        mWorld.mVisible = true;
        mWorld.mObjects.erase(target);
        EXPECT_TRUE(evaluate(p).empty());
    }

    TEST_F(dmoWatchPredicate, InCellBetweenIncludesStartExcludesEndAndWrapsMidnight)
    {
        Predicate p;
        p.mKind = Kind::InCellBetween;
        p.mCell = cell;
        p.mHourFrom = 12;
        p.mHourTo = 14;
        EXPECT_EQ(evaluate(p).size(), 1);
        mWorld.mHour = 14;
        EXPECT_TRUE(evaluate(p).empty());
        p.mHourFrom = 22;
        p.mHourTo = 3;
        for (double hour : { 22., 23.9, 0., 2.9 })
        {
            mWorld.mHour = hour;
            EXPECT_EQ(evaluate(p).size(), 1) << hour;
        }
        for (double hour : { 3., 12., 21.9 })
        {
            mWorld.mHour = hour;
            EXPECT_TRUE(evaluate(p).empty()) << hour;
        }
        p.mHourFrom = p.mHourTo = 12;
        EXPECT_TRUE(evaluate(p).empty());
        p.mHourFrom = 0;
        p.mHourTo = 24;
        EXPECT_EQ(evaluate(p).size(), 1);
        mWorld.mPlayer->mCell = otherCell;
        EXPECT_TRUE(evaluate(p).empty());
    }

    TEST_F(dmoWatchPredicate, CombatStateMatchesEachStateAndRejectsNonActors)
    {
        Predicate p;
        p.mKind = Kind::CombatState;
        p.mRef = actor;
        for (auto state : { CombatState::Calm, CombatState::Engaged, CombatState::Fleeing })
        {
            mWorld.mStates[actor] = state;
            for (auto requested : { CombatState::Calm, CombatState::Engaged, CombatState::Fleeing })
            {
                p.mState = requested;
                EXPECT_EQ(evaluate(p).size(), state == requested ? 1 : 0);
            }
        }
        mWorld.mStates.erase(actor);
        EXPECT_TRUE(evaluate(p).empty());
    }

    TEST_F(dmoWatchPredicate, ActorInCellUsesActorRatherThanPlayer)
    {
        Predicate p;
        p.mKind = Kind::ActorInCell;
        p.mRef = actor;
        p.mCell = cell;
        mWorld.mPlayer->mCell = otherCell;
        EXPECT_EQ(evaluate(p).size(), 1);
        p.mCell = otherCell;
        EXPECT_TRUE(evaluate(p).empty());
        p.mCell = cell;
        mWorld.mStates.erase(actor);
        EXPECT_TRUE(evaluate(p).empty());
    }

    TEST(dmoWatchCombatState, ResolvesPlayerAndActorInputs)
    {
        for (bool inCombat : { false, true })
            for (bool fleeing : { false, true })
            {
                EXPECT_EQ(resolveCombatState(true, false, inCombat, fleeing), CombatState::Calm);
                EXPECT_EQ(resolveCombatState(true, true, inCombat, fleeing), CombatState::Engaged);
            }
        for (bool hasActorsFightingPlayer : { false, true })
        {
            EXPECT_EQ(resolveCombatState(false, hasActorsFightingPlayer, false, false), CombatState::Calm);
            EXPECT_EQ(resolveCombatState(false, hasActorsFightingPlayer, true, false), CombatState::Engaged);
            EXPECT_EQ(resolveCombatState(false, hasActorsFightingPlayer, false, true), CombatState::Fleeing);
            EXPECT_EQ(resolveCombatState(false, hasActorsFightingPlayer, true, true), CombatState::Fleeing);
        }
    }

    TEST_F(dmoWatchPredicate, UnavailableReferencesAreFalseForEveryObjectPredicate)
    {
        mWorld.mObjects.erase(actor);
        for (auto kind : { Kind::Near, Kind::Los, Kind::CombatState, Kind::ActorInCell })
        {
            auto p = nearActor();
            p.mKind = kind;
            p.mTo = target;
            p.mCell = cell;
            EXPECT_TRUE(evaluate(p).empty());
        }
    }

    TEST_F(dmoWatchPredicate, CompositesKeepOnlyBindingsFromSuccessfulBranches)
    {
        Predicate all;
        all.mKind = Kind::All;
        all.mChildren = { nearActor(), nearActor(4) };
        EXPECT_TRUE(evaluate(all).empty());
        Predicate any;
        any.mKind = Kind::Any;
        any.mChildren = { all, nearActor() };
        auto events = evaluate(any);
        ASSERT_EQ(events.size(), 1);
        EXPECT_EQ(events[0].mBindings.size(), 1); // Failed all's first match is rolled back.
        all.mChildren[1].mRadius = 6;
        events = evaluate(all);
        ASSERT_EQ(events.size(), 1);
        EXPECT_EQ(events[0].mBindings.size(), 2);
        any.mChildren = { all, nearActor() };
        events = evaluate(any);
        ASSERT_EQ(events.size(), 1);
        EXPECT_EQ(events[0].mBindings.size(), 3);
        any.mChildren = { nearActor(1), nearActor(2) };
        EXPECT_TRUE(evaluate(any).empty());
        all.mChildren.clear();
        any.mChildren.clear();
        EXPECT_EQ(evaluate(all).size(), 1);
        EXPECT_TRUE(evaluate(any).empty());
    }

    TEST_F(dmoWatchPredicate, OnceWaitsForFirstTrueAndIsRemoved)
    {
        EXPECT_EQ(mManager.registerWatch({ "once", nearActor(4), true }), "once");
        EXPECT_TRUE(mManager.update(mWorld).empty());
        ASSERT_EQ(mManager.list().size(), 1);
        mWorld.mObjects[actor].mPosition = {};
        EXPECT_EQ(mManager.update(mWorld).size(), 1);
        EXPECT_TRUE(mManager.list().empty());
        EXPECT_TRUE(mManager.update(mWorld).empty());
    }

    TEST_F(dmoWatchPredicate, RepeatingFiresOnlyOnFalseToTrueIncludingInitiallyTrue)
    {
        mManager.registerWatch({ "repeat", nearActor() });
        EXPECT_EQ(mManager.update(mWorld).size(), 1);
        EXPECT_TRUE(mManager.update(mWorld).empty());
        mWorld.mObjects.erase(actor);
        EXPECT_TRUE(mManager.update(mWorld).empty());
        mWorld.mObjects[actor] = { {}, cell, cell };
        EXPECT_EQ(mManager.update(mWorld).size(), 1);
        EXPECT_TRUE(mManager.update(mWorld).empty());
        EXPECT_EQ(mManager.list().size(), 1);
    }

    TEST_F(dmoWatchPredicate, AbsoluteExpiryWinsOverFireAndCancelIsSilent)
    {
        mManager.registerWatch({ "expired", nearActor(), true, 100 });
        auto events = mManager.update(mWorld);
        ASSERT_EQ(events.size(), 1);
        EXPECT_TRUE(events[0].mExpired);
        EXPECT_EQ(events[0].mId, "expired");
        EXPECT_TRUE(events[0].mBindings.empty());
        EXPECT_TRUE(mManager.list().empty());
        mManager.registerWatch({ "later", nearActor(1), false, 101 });
        EXPECT_TRUE(mManager.update(mWorld).empty());
        mWorld.mTime = 101;
        events = mManager.update(mWorld);
        ASSERT_EQ(events.size(), 1);
        EXPECT_TRUE(events[0].mExpired);
        mManager.registerWatch({ "cancel", nearActor() });
        mManager.cancel("cancel");
        mManager.cancel("absent");
        EXPECT_TRUE(mManager.update(mWorld).empty());
        EXPECT_TRUE(mManager.list().empty());
    }

    TEST_F(dmoWatchPredicate, ListCopiesSpecsAndReplacementResetsTheEdge)
    {
        mManager.registerWatch({ "watch", nearActor(), false, 110 });
        auto list = mManager.list();
        ASSERT_EQ(list.size(), 1);
        EXPECT_EQ(list[0].mId, "watch");
        EXPECT_EQ(list[0].mPredicate.mRef, actor);
        EXPECT_FALSE(list[0].mOnce);
        EXPECT_EQ(list[0].mExpiresAt, 110);
        list[0].mPredicate.mRadius = 0;
        EXPECT_EQ(mManager.update(mWorld).size(), 1);
        EXPECT_TRUE(mManager.update(mWorld).empty());
        mManager.registerWatch({ "watch", nearActor(), true });
        EXPECT_EQ(mManager.list().size(), 1);
        EXPECT_EQ(mManager.update(mWorld).size(), 1);
        EXPECT_TRUE(mManager.list().empty());
    }

    TEST_F(dmoWatchPredicate, ClearUsedByLoadAndNewGameDropsSetAndEdgeState)
    {
        // openmw-tests has no full game save/load harness. LuaManager::clear calls this operation.
        mManager.registerWatch({ "repeat", nearActor() });
        mManager.registerWatch({ "waiting", nearActor(1), true, 101 });
        EXPECT_EQ(mManager.update(mWorld).size(), 1);
        mManager.clear();
        EXPECT_TRUE(mManager.list().empty());
        mWorld.mTime = 102;
        EXPECT_TRUE(mManager.update(mWorld).empty());
        mManager.registerWatch({ "repeat", nearActor() });
        EXPECT_EQ(mManager.update(mWorld).size(), 1);
    }

    TEST_F(dmoWatchPredicate, ThousandNearWatchesEvaluateUnderOneMillisecondPerFrame)
    {
        for (int i = 0; i < 1000; ++i)
            mManager.registerWatch({ std::to_string(i), nearActor() });
        ASSERT_EQ(mManager.update(mWorld).size(), 1000);
        constexpr int frames = 1000;
        std::size_t events = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < frames; ++frame)
            events += mManager.update(mWorld).size();
        const double microseconds
            = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / frames;
        std::cout << "1000 near watches: " << microseconds << " microseconds per frame\n";
        EXPECT_EQ(events, 0);
        EXPECT_EQ(mManager.list().size(), 1000);
        EXPECT_LT(microseconds, 1000.0);
    }

    class dmoWatchBindings : public testing::Test
    {
    protected:
        Manager mManager;
        LuaUtil::LuaState mLua{ nullptr, nullptr };

        void SetUp() override
        {
            mLua.protectedCall([&](LuaUtil::LuaView& view) {
                auto object = view.sol().new_usertype<MWLua::GObject>(
                    "GObject", sol::no_constructor, sol::base_classes, sol::bases<MWLua::Object>());
                object[sol::meta_function::equal_to] = [](const MWLua::GObject& a, const MWLua::GObject& b) {
                    return a.id() == b.id();
                };
                view.sol()["actor"] = MWLua::GObject(actor);
                view.sol()["target"] = MWLua::GObject(target);
                view.sol()["watch"] = MWLua::initWatchPackage(view.sol(), mManager);
            });
        }

        void run(std::string_view script)
        {
            std::string error;
            mLua.protectedCall([&](LuaUtil::LuaView& view) {
                auto result = view.sol().safe_script(script, sol::script_pass_on_error);
                if (!result.valid())
                    error = result.get<sol::error>().what();
            });
            ASSERT_TRUE(error.empty()) << error;
        }
    };

    TEST_F(dmoWatchBindings, RegisterAndListRoundTripAllPredicateShapes)
    {
        run(R"(
            local predicates = {
                {near={ref=actor, radius=5}}, {near={cell='hall', radius=0}},
                {los={from=actor, to=target}},
                {inCellBetween={cell='hall', hourFrom=22, hourTo=3}},
                {combatState={ref=actor, state='calm'}}, {actorInCell={ref=actor, cell='hall'}},
                {all={{near={ref=actor, radius=5}}, {any={{near={cell='hall', radius=0}}}}}},
                {any={{near={ref=actor, radius=1}}, {actorInCell={ref=actor, cell='hall'}}}}
            }
            for i, predicate in ipairs(predicates) do
                local id = tostring(i)
                assert(watch.register{id=id, predicate=predicate, once=true, expiryGameHours=101} == id)
            end
            local function equal(a, b)
                if type(a) ~= 'table' then return a == b end
                for k, v in pairs(a) do if not equal(v, b[k]) then return false end end
                for k in pairs(b) do if a[k] == nil then return false end end
                return true
            end
            local entries = watch.list()
            assert(#entries == #predicates)
            for i, entry in ipairs(entries) do
                assert(entry.id == tostring(i) and entry.once and entry.expiresAt == 101)
                assert(equal(entry.predicate, predicates[i]))
            end
        )");
        const auto specs = mManager.list();
        ASSERT_EQ(specs.size(), 8);
        EXPECT_EQ(specs[0].mPredicate.mRef, actor);
        EXPECT_EQ(specs[2].mPredicate.mRef, actor);
        EXPECT_EQ(specs[2].mPredicate.mTo, target);
        EXPECT_EQ(specs[4].mPredicate.mRef, actor);
        EXPECT_EQ(specs[5].mPredicate.mRef, actor);
        FixtureWorld world;
        world.mHour = 23;
        EXPECT_EQ(mManager.update(world).size(), 8);
        EXPECT_TRUE(mManager.list().empty());
    }

    TEST_F(dmoWatchBindings, RegistrationAndListDoNotRetainMutableLuaTables)
    {
        run(R"(
            local spec = {id='stable', predicate={near={ref=actor, radius=5}}}
            watch.register(spec)
            spec.predicate.near.radius = 0
            local copy = watch.list()[1]
            assert(copy.once == false and copy.expiresAt == nil and copy.predicate.near.radius == 5)
            copy.predicate.near.radius = 0
            assert(watch.list()[1].predicate.near.radius == 5)
            assert(not pcall(function() watch.register = nil end))
        )");
        EXPECT_EQ(mManager.update(FixtureWorld()).size(), 1);
        run("watch.cancel('stable'); watch.cancel('absent'); assert(#watch.list() == 0)");
    }

    TEST_F(dmoWatchBindings, InvalidSpecsNameTheFieldAndDoNotReplaceExistingWatch)
    {
        run(R"(
            watch.register{id='kept', predicate={all={}}}
            local function bad(spec, field)
                local ok, err = pcall(watch.register, spec)
                assert(not ok, 'accepted invalid ' .. field)
                assert(string.find(err, field, 1, true), err)
                assert(#watch.list() == 1 and watch.list()[1].id == 'kept')
            end
            local function predicate(p, field) bad({id='kept', predicate=p}, field) end
            bad({}, 'id')
            bad({id=3, predicate={all={}}}, 'id')
            bad({id='kept'}, 'predicate')
            bad({id='kept', predicate={all={}}, once='yes'}, 'once')
            bad({id='kept', predicate={all={}}, expiryGameHours='later'}, 'expiryGameHours')
            predicate({mystery={}}, 'mystery')
            predicate({}, 'predicate')
            predicate({all={}, any={}}, 'predicate')
            predicate({near={ref=actor}}, 'radius')
            predicate({near={ref=actor, radius='5'}}, 'radius')
            predicate({near={radius=1}}, 'ref/cell')
            predicate({near={ref=actor, cell='hall', radius=1}}, 'ref/cell')
            predicate({near={ref='actor', radius=1}}, 'ref')
            predicate({near={cell=7, radius=1}}, 'cell')
            predicate({los={from=actor}}, 'to')
            predicate({los={to=target}}, 'from')
            predicate({inCellBetween={cell='hall', hourTo=3}}, 'hourFrom')
            predicate({inCellBetween={cell='hall', hourFrom='noon', hourTo=3}}, 'hourFrom')
            predicate({combatState={ref=actor, state='asleep'}}, 'state')
            predicate({actorInCell={ref=actor}}, 'cell')
        )");
    }

    TEST_F(dmoWatchBindings, UnknownCellNameIsRetainedAndDoesNotMatch)
    {
        run(R"(
            watch.register{id='unknown', predicate={near={cell='dmo nonexistent watch cell', radius=0}}}
            assert(watch.list()[1].predicate.near.cell == 'dmo nonexistent watch cell')
        )");
        EXPECT_TRUE(mManager.update(FixtureWorld()).empty());
    }

    TEST_F(dmoWatchBindings, ExtraPredicateFieldsAreIgnoredAndDeepCompositesAreAccepted)
    {
        run(R"(
            local p = {inCellBetween={cell='hall', hourFrom=0, hourTo=24, ref=actor, extra=true}}
            p = {all={p, note='ignored'}}
            for i = 1, 40 do p = {all={p}} end
            watch.register{id='deep', predicate=p, once=true}
        )");
        EXPECT_EQ(mManager.update(FixtureWorld()).size(), 1);
    }

    TEST_F(dmoWatchBindings, FiredEventSerializesReferencesCellsAndEmptyBindings)
    {
        EXPECT_STREQ(sFiredEvent, "WatchFired");
        Predicate los;
        los.mKind = Kind::Los;
        los.mRef = actor;
        los.mTo = target;
        Predicate all;
        all.mKind = Kind::All;
        all.mChildren = { nearActor(), los };
        mManager.registerWatch({ "fired", all, true });
        const auto events = mManager.update(FixtureWorld());
        ASSERT_EQ(events.size(), 1);
        auto serializer = MWLua::createUserdataSerializer(false);
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            const auto event = MWLua::makeWatchEvent(view.sol(), events[0], serializer.get());
            EXPECT_EQ(event.mEventName, sFiredEvent);
            view.sol()["fired"] = LuaUtil::deserialize(view.sol(), event.mEventData, serializer.get());
            const auto empty = MWLua::makeWatchEvent(view.sol(), { "empty", false, {} }, serializer.get());
            view.sol()["empty"] = LuaUtil::deserialize(view.sol(), empty.mEventData, serializer.get());
        });
        run(R"(
            assert(fired.id == 'fired' and #fired.bindings == 2)
            assert(fired.bindings[1].ref == actor and fired.bindings[1].cell == 'hall')
            assert(fired.bindings[1].from == nil and fired.bindings[1].to == nil)
            assert(fired.bindings[2].from == actor and fired.bindings[2].to == target)
            assert(fired.bindings[2].ref == nil and fired.bindings[2].cell == nil)
            assert(empty.id == 'empty' and type(empty.bindings) == 'table' and #empty.bindings == 0)
        )");
    }

    TEST_F(dmoWatchBindings, ExpiredEventSerializesOnlyId)
    {
        EXPECT_STREQ(sExpiredEvent, "WatchExpired");
        mManager.registerWatch({ "expired", nearActor(), true, 100 });
        const auto events = mManager.update(FixtureWorld());
        ASSERT_EQ(events.size(), 1);
        auto serializer = MWLua::createUserdataSerializer(false);
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            const auto event = MWLua::makeWatchEvent(view.sol(), events[0], serializer.get());
            EXPECT_EQ(event.mEventName, sExpiredEvent);
            view.sol()["expired"] = LuaUtil::deserialize(view.sol(), event.mEventData, serializer.get());
        });
        run(R"(
            assert(expired.id == 'expired' and expired.bindings == nil)
            for key in pairs(expired) do assert(key == 'id') end
        )");
    }

    TEST_F(dmoWatchBindings, ApiExtensionsAdvertisesWatchAndRejectsWrites)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["extensions"] = MWLua::initCoreApiExtensions(view.sol());
        });
        run(R"(
            assert(extensions.watch == 1)
            assert(not pcall(function() extensions.watch = 2 end))
            assert(not pcall(function() extensions.other = 1 end))
            assert(extensions.watch == 1 and extensions.other == nil)
        )");
    }

    TEST_F(dmoWatchBindings, ExteriorCellIdMatchesAndUsesSerializedFormInBindings)
    {
        FixtureWorld world;
        world.mPlayer->mCell = ESM::RefId(ESM::ESM3ExteriorCellRefId(-2, 5));
        run(R"(
            watch.register{id='exterior', predicate={near={cell='Esm3ExteriorCell:-2:5', radius=0}}, once=true}
            assert(watch.list()[1].predicate.near.cell == 'Esm3ExteriorCell:-2:5')
        )");
        const auto events = mManager.update(world);
        ASSERT_EQ(events.size(), 1);
        auto serializer = MWLua::createUserdataSerializer(false);
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            const auto event = MWLua::makeWatchEvent(view.sol(), events[0], serializer.get());
            EXPECT_EQ(event.mEventName, sFiredEvent);
            view.sol()["exterior"] = LuaUtil::deserialize(view.sol(), event.mEventData, serializer.get());
        });
        run(R"(
            assert(exterior.id == 'exterior' and #exterior.bindings == 1)
            assert(exterior.bindings[1].cell == 'Esm3ExteriorCell:-2:5' and exterior.bindings[1].ref == nil)
        )");
    }
}
