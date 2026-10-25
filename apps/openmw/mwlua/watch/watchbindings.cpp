#include "watchbindings.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <components/lua/luastate.hpp>
#include <components/lua/serialization.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/mechanicsmanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwmechanics/aisequence.hpp"
#include "../../mwmechanics/creaturestats.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/class.hpp"
#include "../../mwworld/datetimemanager.hpp"
#include "../../mwworld/timestamp.hpp"
#include "../../mwworld/worldmodel.hpp"
#include "../context.hpp"
#include "../luamanagerimp.hpp"
#include "../object.hpp"
#include "watchmanager.hpp"

namespace MWLua
{
    namespace
    {
        using Kind = Watch::Predicate::Kind;
        constexpr std::string_view kinds[]
            = { "near", "los", "inCellBetween", "combatState", "actorInCell", "all", "any" };
        constexpr std::string_view states[] = { "engaged", "fleeing", "calm" };

        [[noreturn]] void invalid(const std::string& field, std::string_view reason)
        {
            throw std::runtime_error("openmw.watch: " + field + " " + std::string(reason));
        }

        template <class T>
        T required(const sol::object& value, const std::string& field)
        {
            if constexpr (std::is_same_v<T, std::string>)
            {
                if (value.get_type() != sol::type::string)
                    invalid(field, "must be a string");
            }
            if constexpr (std::is_same_v<T, double>)
            {
                if (value.get_type() != sol::type::number)
                    invalid(field, "must be a number");
            }
            if (!value.is<T>())
                invalid(field, "is missing or has the wrong type");
            return value.as<T>();
        }

        Watch::Predicate parsePredicate(const sol::object& value, const std::string& path)
        {
            const auto table = required<sol::table>(value, path);
            Watch::Predicate p;
            int count = 0;
            for (const auto& [key, args] : table)
            {
                const auto name = required<std::string>(key, path + ".key");
                const auto it = std::find(std::begin(kinds), std::end(kinds), name);
                if (it == std::end(kinds))
                    invalid(path + "." + name, "is unknown");
                p.mKind = static_cast<Kind>(it - std::begin(kinds));
                ++count;
            }
            if (count != 1)
                invalid(path, "must contain exactly one predicate key");
            const std::string field = path + "." + std::string(kinds[static_cast<int>(p.mKind)]);
            const auto args = required<sol::table>(table[kinds[static_cast<int>(p.mKind)]], field);
            if (p.mKind == Kind::All || p.mKind == Kind::Any)
            {
                for (std::size_t i = 1; i <= args.size(); ++i)
                    p.mChildren.push_back(parsePredicate(args[i], field + "[" + std::to_string(i) + "]"));
                return p;
            }
            if (p.mKind == Kind::Near)
            {
                if ((args["ref"] != sol::nil) == (args["cell"] != sol::nil))
                    invalid(field + ".ref/cell", "requires exactly one target");
                p.mRadius = required<double>(args["radius"], field + ".radius");
            }
            if (p.mKind == Kind::CombatState || p.mKind == Kind::ActorInCell
                || (p.mKind == Kind::Near && args["ref"] != sol::nil))
                p.mRef = required<GObject>(args["ref"], field + ".ref").id();
            if (p.mKind == Kind::Los)
            {
                p.mRef = required<GObject>(args["from"], field + ".from").id();
                p.mTo = required<GObject>(args["to"], field + ".to").id();
            }
            if (p.mKind == Kind::InCellBetween || p.mKind == Kind::ActorInCell || (p.mKind == Kind::Near && !p.mRef))
            {
                const auto cell = required<std::string>(args["cell"], field + ".cell");
                p.mCell = ESM::RefId::deserializeText(cell);
                if (p.mCell.empty())
                    p.mCell = ESM::RefId::stringRefId(cell);
            }
            if (p.mKind == Kind::InCellBetween)
            {
                p.mHourFrom = required<double>(args["hourFrom"], field + ".hourFrom");
                p.mHourTo = required<double>(args["hourTo"], field + ".hourTo");
            }
            if (p.mKind == Kind::CombatState)
            {
                const auto state = required<std::string>(args["state"], field + ".state");
                const auto it = std::find(std::begin(states), std::end(states), state);
                if (it == std::end(states))
                    invalid(field + ".state", "must be engaged, fleeing, or calm");
                p.mState = static_cast<Watch::CombatState>(it - std::begin(states));
            }
            return p;
        }

        sol::table predicateTable(sol::state_view lua, const Watch::Predicate& p)
        {
            sol::table result(lua, sol::create), args(lua, sol::create);
            if (p.mKind == Kind::All || p.mKind == Kind::Any)
            {
                for (const auto& child : p.mChildren)
                    args.add(predicateTable(lua, child));
            }
            else
            {
                if (p.mRef)
                    args[p.mKind == Kind::Los ? "from" : "ref"] = GObject(*p.mRef);
                if (!p.mCell.empty())
                    args["cell"] = p.mCell.serializeText();
                if (p.mKind == Kind::Near)
                    args["radius"] = p.mRadius;
                if (p.mKind == Kind::Los)
                    args["to"] = GObject(p.mTo);
                if (p.mKind == Kind::InCellBetween)
                {
                    args["hourFrom"] = p.mHourFrom;
                    args["hourTo"] = p.mHourTo;
                }
                if (p.mKind == Kind::CombatState)
                    args["state"] = states[static_cast<int>(p.mState)];
            }
            result[kinds[static_cast<int>(p.mKind)]] = args;
            return result;
        }

        class EngineWorld final : public Watch::World
        {
            // Each update constructs a new EngineWorld.
            mutable std::optional<Watch::CombatState> mPlayerCombatState;

            static MWWorld::Ptr find(ESM::RefNum id)
            {
                const auto ptr = MWBase::Environment::get().getWorldModel()->getPtr(id);
                return ptr.isEmpty() || ptr.mRef->isDeleted() ? MWWorld::Ptr() : ptr;
            }
            static std::optional<Watch::Location> location(const MWWorld::Ptr& ptr)
            {
                if (ptr.isEmpty() || !ptr.isInCell() || ptr.mRef->isDeleted())
                    return std::nullopt;
                const auto* cell = ptr.getCell()->getCell();
                return Watch::Location{ osg::Vec3d(ptr.getRefData().getPosition().asVec3()), cell->getId(),
                    cell->getWorldSpace() };
            }

        public:
            std::optional<Watch::Location> player() const override
            {
                return location(MWBase::Environment::get().getWorld()->getPlayerPtr());
            }
            std::optional<Watch::Location> object(ESM::RefNum ref) const override { return location(find(ref)); }
            bool lineOfSight(ESM::RefNum from, ESM::RefNum to) const override
            {
                const auto a = find(from), b = find(to);
                return !a.isEmpty() && !b.isEmpty() && MWBase::Environment::get().getWorld()->getLOS(a, b);
            }
            std::optional<Watch::CombatState> combatState(ESM::RefNum ref) const override
            {
                const auto ptr = find(ref);
                if (ptr.isEmpty() || !ptr.getClass().isActor())
                    return std::nullopt;
                if (ptr == MWBase::Environment::get().getWorld()->getPlayerPtr())
                {
                    if (!mPlayerCombatState)
                        mPlayerCombatState = Watch::resolveCombatState(true,
                            !MWBase::Environment::get().getMechanicsManager()->getActorsFighting(ptr).empty(), false,
                            false);
                    return mPlayerCombatState;
                }
                const auto& ai = ptr.getClass().getCreatureStats(ptr).getAiSequence();
                return Watch::resolveCombatState(false, false, ai.isInCombat(), ai.isFleeing());
            }
            double gameHour() const override
            {
                return MWBase::Environment::get().getWorld()->getTimeManager()->getTimeStamp().getHour();
            }
            double gameTimeHours() const override
            {
                return MWBase::Environment::get().getWorld()->getTimeManager()->getGameTime() / 3600.0;
            }
        };
    }

    sol::table initWatchPackage(sol::state_view lua, Watch::Manager& manager)
    {
        sol::table api(lua, sol::create);
        api["register"] = [&manager](const sol::object& value) {
            const auto table = required<sol::table>(value, "spec");
            Watch::Spec spec;
            spec.mId = required<std::string>(table["id"], "id");
            spec.mPredicate = parsePredicate(table["predicate"], "predicate");
            if (table["once"] != sol::nil)
                spec.mOnce = required<bool>(table["once"], "once");
            if (table["expiryGameHours"] != sol::nil)
                spec.mExpiresAt = required<double>(table["expiryGameHours"], "expiryGameHours");
            return manager.registerWatch(std::move(spec));
        };
        api["cancel"] = [&manager](const std::string& id) { manager.cancel(id); };
        api["list"] = [lua, &manager] {
            sol::table result(lua, sol::create);
            for (const auto& spec : manager.list())
            {
                sol::table entry(lua, sol::create);
                entry["id"] = spec.mId;
                entry["predicate"] = predicateTable(lua, spec.mPredicate);
                entry["once"] = spec.mOnce;
                if (spec.mExpiresAt)
                    entry["expiresAt"] = *spec.mExpiresAt;
                result.add(entry);
            }
            return result;
        };
        return LuaUtil::makeReadOnly(api);
    }

    sol::table initWatchPackage(const Context& context)
    {
        return initWatchPackage(context.sol(), context.mLuaManager->watches());
    }

    LuaEvents::Global makeWatchEvent(
        sol::state_view lua, const Watch::Event& event, const LuaUtil::UserdataSerializer* serializer)
    {
        sol::table data = lua.create_table_with("id", event.mId);
        if (!event.mExpired)
        {
            sol::table bindings(lua, sol::create);
            for (const auto& match : event.mBindings)
            {
                sol::table binding(lua, sol::create);
                if (match.mRef)
                    binding["ref"] = GObject(*match.mRef);
                if (match.mFrom)
                    binding["from"] = GObject(*match.mFrom);
                if (match.mTo)
                    binding["to"] = GObject(*match.mTo);
                if (!match.mCell.empty())
                    binding["cell"] = match.mCell.serializeText();
                bindings.add(binding);
            }
            data["bindings"] = bindings;
        }
        return { event.mExpired ? Watch::sExpiredEvent : Watch::sFiredEvent, LuaUtil::serialize(data, serializer) };
    }

    void updateWatches(Watch::Manager& manager, LuaUtil::LuaState& lua, LuaEvents& events,
        const LuaUtil::UserdataSerializer* serializer)
    {
        const auto batch = manager.update(EngineWorld());
        if (batch.empty())
            return;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            for (const auto& event : batch)
                events.addGlobalEvent(makeWatchEvent(view.sol(), event, serializer));
        });
    }
}
