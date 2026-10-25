#include "watchmanager.hpp"

#include <utility>

namespace MWLua::Watch
{
    CombatState resolveCombatState(bool isPlayer, bool hasActorsFightingPlayer, bool inCombat, bool fleeing)
    {
        if (isPlayer)
            return hasActorsFightingPlayer ? CombatState::Engaged : CombatState::Calm;
        if (fleeing)
            return CombatState::Fleeing;
        return inCombat ? CombatState::Engaged : CombatState::Calm;
    }

    namespace
    {
        bool evaluate(const Predicate& p, const World& world, std::vector<Binding>& bindings)
        {
            using Kind = Predicate::Kind;
            if (p.mKind == Kind::All || p.mKind == Kind::Any)
            {
                const auto start = bindings.size();
                bool matched = p.mKind == Kind::All;
                for (const Predicate& child : p.mChildren)
                {
                    const bool value = evaluate(child, world, bindings);
                    if (p.mKind == Kind::All && !value)
                    {
                        bindings.resize(start);
                        return false;
                    }
                    matched = matched || value;
                }
                return matched;
            }

            Binding binding;
            if (p.mKind == Kind::Los)
            {
                if (!world.object(*p.mRef) || !world.object(p.mTo) || !world.lineOfSight(*p.mRef, p.mTo))
                    return false;
                binding.mFrom = p.mRef;
                binding.mTo = p.mTo;
            }
            else
            {
                const auto location = p.mRef ? world.object(*p.mRef) : world.player();
                if (!location)
                    return false;
                switch (p.mKind)
                {
                    case Kind::Near:
                        if (p.mRef)
                        {
                            const auto player = world.player();
                            if (!player || player->mWorldSpace != location->mWorldSpace
                                || (player->mPosition - location->mPosition).length2() > p.mRadius * p.mRadius)
                                return false;
                        }
                        else if (location->mCell != p.mCell)
                            return false;
                        break;
                    case Kind::InCellBetween:
                    {
                        const double hour = world.gameHour();
                        const bool within = p.mHourFrom > p.mHourTo
                            ? hour >= p.mHourFrom || hour < p.mHourTo
                            : hour >= p.mHourFrom && hour < p.mHourTo;
                        if (location->mCell != p.mCell || !within)
                            return false;
                        break;
                    }
                    case Kind::CombatState:
                        if (world.combatState(*p.mRef) != p.mState)
                            return false;
                        break;
                    case Kind::ActorInCell:
                        if (location->mCell != p.mCell || !world.combatState(*p.mRef))
                            return false;
                        break;
                    default:
                        return false;
                }
                binding.mRef = p.mRef;
                binding.mCell = location->mCell;
            }
            bindings.push_back(std::move(binding));
            return true;
        }
    }

    std::string Manager::registerWatch(Spec spec)
    {
        const std::string id = spec.mId;
        mWatches.insert_or_assign(id, Entry{ std::move(spec) });
        return id;
    }

    std::vector<Spec> Manager::list() const
    {
        std::vector<Spec> result;
        result.reserve(mWatches.size());
        for (const auto& [id, entry] : mWatches)
            result.push_back(entry.mSpec);
        return result;
    }

    std::vector<Event> Manager::update(const World& world)
    {
        std::vector<Event> events;
        std::vector<Binding> bindings;
        const double now = world.gameTimeHours();
        for (auto it = mWatches.begin(); it != mWatches.end();)
        {
            auto& [spec, wasTrue] = it->second;
            if (spec.mExpiresAt && now >= *spec.mExpiresAt)
            {
                events.push_back({ spec.mId, true, {} });
                it = mWatches.erase(it);
                continue;
            }
            bindings.clear();
            const bool matched = evaluate(spec.mPredicate, world, bindings);
            if (matched && !wasTrue)
            {
                events.push_back({ spec.mId, false, bindings });
                if (spec.mOnce)
                {
                    it = mWatches.erase(it);
                    continue;
                }
            }
            wasTrue = matched;
            ++it;
        }
        return events;
    }
}
