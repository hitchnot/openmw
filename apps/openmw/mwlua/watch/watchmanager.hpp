#ifndef MWLUA_WATCHMANAGER_H
#define MWLUA_WATCHMANAGER_H

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <osg/Vec3d>

#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>

namespace MWLua::Watch
{
    enum class CombatState
    {
        Engaged,
        Fleeing,
        Calm
    };

    CombatState resolveCombatState(bool isPlayer, bool hasActorsFightingPlayer, bool inCombat, bool fleeing);

    struct Location
    {
        osg::Vec3d mPosition;
        ESM::RefId mCell;
        ESM::RefId mWorldSpace;
    };

    class World
    {
    public:
        virtual ~World() = default;
        virtual std::optional<Location> player() const = 0;
        virtual std::optional<Location> object(ESM::RefNum ref) const = 0;
        virtual bool lineOfSight(ESM::RefNum from, ESM::RefNum to) const = 0;
        // A missing value also identifies non-actors.
        virtual std::optional<CombatState> combatState(ESM::RefNum ref) const = 0;
        virtual double gameHour() const = 0;
        virtual double gameTimeHours() const = 0;
    };

    struct Predicate
    {
        enum class Kind
        {
            Near,
            Los,
            InCellBetween,
            CombatState,
            ActorInCell,
            All,
            Any
        };
        Kind mKind = Kind::Near;
        std::optional<ESM::RefNum> mRef;
        ESM::RefNum mTo;
        ESM::RefId mCell;
        double mRadius = 0;
        double mHourFrom = 0;
        double mHourTo = 0;
        CombatState mState = CombatState::Calm;
        std::vector<Predicate> mChildren;
    };

    struct Binding
    {
        std::optional<ESM::RefNum> mRef;
        std::optional<ESM::RefNum> mFrom;
        std::optional<ESM::RefNum> mTo;
        ESM::RefId mCell;
    };

    struct Spec
    {
        std::string mId;
        Predicate mPredicate;
        bool mOnce = false;
        std::optional<double> mExpiresAt;
    };

    struct Event
    {
        std::string mId;
        bool mExpired;
        std::vector<Binding> mBindings;
    };

    class Manager
    {
    public:
        std::string registerWatch(Spec spec);
        void cancel(const std::string& id) { mWatches.erase(id); }
        std::vector<Spec> list() const;
        std::vector<Event> update(const World& world);
        void clear() { mWatches.clear(); }

    private:
        struct Entry
        {
            Spec mSpec;
            bool mWasTrue = false;
        };
        std::map<std::string, Entry> mWatches;
    };
}

#endif
