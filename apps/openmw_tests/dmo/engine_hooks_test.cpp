#include <apps/openmw/mwlua/enginehooks.hpp>

#include <gtest/gtest.h>

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
