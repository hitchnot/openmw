#ifndef OPENMW_COMPONENTS_IPC_QUEUE_H
#define OPENMW_COMPONENTS_IPC_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ipc
{
    inline constexpr std::size_t sOutboundCapacity = 1024;
    inline constexpr std::size_t sInboundCapacity = 4096;
    inline constexpr std::size_t sDrainPerFrame = 256;

    class Queue
    {
    public:
        explicit Queue(std::size_t capacity);
        bool tryPush(std::string line);
        void pushDroppingOldest(std::string line);
        std::optional<std::string> pop();
        std::vector<std::string> drain(std::size_t max);
        std::uint64_t dropped() const;
        void clear();

    private:
        const std::size_t mCapacity;
        mutable std::mutex mMutex;
        std::deque<std::string> mLines;
        std::uint64_t mDropped = 0;
    };

    std::vector<std::string> drainFrame(Queue& queue, std::size_t max = sDrainPerFrame);

    class LineSplitter
    {
    public:
        std::vector<std::string> append(std::string_view bytes);

    private:
        std::string mPartial;
    };
}

#endif
