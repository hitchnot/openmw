#ifndef MWLUA_WATCHBINDINGS_H
#define MWLUA_WATCHBINDINGS_H

#include <sol/sol.hpp>

#include "../luaevents.hpp"

namespace LuaUtil
{
    class LuaState;
    class UserdataSerializer;
}

namespace MWLua
{
    struct Context;
    namespace Watch
    {
        class Manager;
        struct Event;
        inline constexpr char sFiredEvent[] = "WatchFired";
        inline constexpr char sExpiredEvent[] = "WatchExpired";
    }

    sol::table initWatchPackage(sol::state_view lua, Watch::Manager& manager);
    sol::table initWatchPackage(const Context& context);
    LuaEvents::Global makeWatchEvent(
        sol::state_view lua, const Watch::Event& event, const LuaUtil::UserdataSerializer* serializer);
    void updateWatches(Watch::Manager& manager, LuaUtil::LuaState& lua, LuaEvents& events,
        const LuaUtil::UserdataSerializer* serializer);
}

#endif
