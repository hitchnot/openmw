#include "queue.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Ipc
{
    Queue::Queue(std::size_t capacity)
        : mCapacity(capacity)
    {
        if (capacity == 0)
            throw std::invalid_argument("IPC queue capacity must be positive");
    }

    bool Queue::tryPush(std::string line)
    {
        const std::lock_guard lock(mMutex);
        if (mLines.size() == mCapacity)
            return false;
        mLines.push_back(std::move(line));
        return true;
    }

    void Queue::pushDroppingOldest(std::string line)
    {
        const std::lock_guard lock(mMutex);
        if (mLines.size() == mCapacity)
        {
            mLines.pop_front();
            ++mDropped;
        }
        mLines.push_back(std::move(line));
    }

    std::optional<std::string> Queue::pop()
    {
        const std::lock_guard lock(mMutex);
        if (mLines.empty())
            return std::nullopt;
        std::string line = std::move(mLines.front());
        mLines.pop_front();
        return line;
    }

    std::vector<std::string> Queue::drain(std::size_t max)
    {
        const std::lock_guard lock(mMutex);
        std::vector<std::string> result;
        max = std::min(max, mLines.size());
        result.reserve(max);
        while (result.size() < max)
        {
            result.push_back(std::move(mLines.front()));
            mLines.pop_front();
        }
        return result;
    }

    std::uint64_t Queue::dropped() const
    {
        const std::lock_guard lock(mMutex);
        return mDropped;
    }

    void Queue::clear()
    {
        const std::lock_guard lock(mMutex);
        mLines.clear();
    }

    std::vector<std::string> drainFrame(Queue& queue, std::size_t max)
    {
        return queue.drain(std::min(max, sDrainPerFrame));
    }

    std::vector<std::string> LineSplitter::append(std::string_view bytes)
    {
        std::vector<std::string> result;
        std::size_t start = 0;
        for (std::size_t end = bytes.find('\n'); end != std::string_view::npos; end = bytes.find('\n', start))
        {
            mPartial.append(bytes.substr(start, end - start));
            if (!mPartial.empty() && mPartial.back() == '\r')
                mPartial.pop_back();
            result.push_back(std::move(mPartial));
            mPartial.clear();
            start = end + 1;
        }
        mPartial.append(bytes.substr(start));
        return result;
    }
}
