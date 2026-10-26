#include <components/esm/luascripts.hpp>
#include <components/lua/configuration.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/scriptscontainer.hpp>
#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include <utility>

namespace
{
    TEST(dmoLuaTimers, CallbackCanScheduleOverdueRealTimeTimerWithoutRepeatingOrDroppingTimers)
    {
        constexpr VFS::Path::NormalizedView path("timers.lua");
        TestingOpenMW::VFSTestFile script("return {}");
        auto vfs = TestingOpenMW::createTestVFS({ { path, &script } });
        ESM::LuaScriptsCfg config;
        LuaUtil::parseOMWScripts(config, "CUSTOM: timers.lua\n");
        LuaUtil::ScriptsConfiguration scriptsConfig;
        scriptsConfig.init(std::move(config), false);
        LuaUtil::LuaState lua(vfs.get(), &scriptsConfig);
        LuaUtil::ScriptsContainer scripts(&lua, "Test");
        const auto scriptId = scriptsConfig.findId(path);
        ASSERT_TRUE(scriptId.has_value());
        ASSERT_TRUE(scripts.addCustomScript(*scriptId));

        using TimerType = LuaUtil::ScriptsContainer::TimerType;
        int firstCalls = 0;
        int overdueCalls = 0;
        sol::function overdue = sol::make_object(lua.unsafeState(), [&] { ++overdueCalls; });
        sol::function first = sol::make_object(lua.unsafeState(), [&] {
            ++firstCalls;
            // Keep a duplicate invocation bounded so the regression fails instead of looping.
            if (firstCalls == 1)
                scripts.setupSerializableTimer(
                    TimerType::REAL_TIME, 1, *scriptId, "overdue", sol::make_object(lua.unsafeState(), 0));
        });
        scripts.registerTimerCallback(*scriptId, "first", first);
        scripts.registerTimerCallback(*scriptId, "overdue", overdue);
        scripts.setupSerializableTimer(
            TimerType::REAL_TIME, 2, *scriptId, "first", sol::make_object(lua.unsafeState(), 0));

        scripts.processTimers(0, 0, 2);
        EXPECT_EQ(firstCalls, 1);
        EXPECT_EQ(overdueCalls, 1);

        scripts.processTimers(0, 0, 3);
        EXPECT_EQ(firstCalls, 1);
        EXPECT_EQ(overdueCalls, 1);
    }
}
