#ifndef MWLUA_ENGINEHOOKS_H
#define MWLUA_ENGINEHOOKS_H

#include <cstddef>

namespace MWLua
{
    // Caps the engine hooks delivered to the player script in one frame; the excess is counted and dropped.
    class EngineHookGate
    {
    public:
        static constexpr int sPerFrame = 64;

        bool admit()
        {
            if (mThisFrame >= sPerFrame)
            {
                ++mDropped;
                return false;
            }
            ++mThisFrame;
            return true;
        }

        void newFrame() { mThisFrame = 0; }

        std::size_t dropped() const { return mDropped; }

    private:
        int mThisFrame = 0;
        std::size_t mDropped = 0;
    };
}

#endif
