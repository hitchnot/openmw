#ifndef OPENMW_COMPONENTS_IPC_CONNECTION_H
#define OPENMW_COMPONENTS_IPC_CONNECTION_H

#include "queue.hpp"

#include <atomic>
#include <memory>
#include <string_view>

namespace Ipc
{
    class Worker;

    bool isLoopback(std::string_view host);
    void validateEndpoint(std::string_view host, int port);

    enum class State
    {
        Connecting,
        Open,
        Closed
    };

    // Transport data only. No Lua objects may be held here or by Worker.
    class Connection
    {
    public:
        explicit Connection(std::size_t inboundCapacity = sInboundCapacity);
        ~Connection();
        void start(const std::string& host, int port);
        void close();
        State state() const { return mState.load(); }
        bool send(std::string line);
        std::vector<std::string> drain(std::size_t max);
        std::uint64_t sent() const { return mSent.load(); }
        std::uint64_t received() const { return mReceived.load(); }
        std::uint64_t dropped() const { return mInbound.dropped(); }

        // Worker operations, also used by the transport-free tests.
        void opened();
        void finished();
        void receive(std::string line);
        std::optional<std::string> takeOutbound() { return mOutbound.pop(); }
        void didSend() { ++mSent; }

    private:
        std::atomic<State> mState{ State::Connecting };
        std::atomic<std::uint64_t> mSent{ 0 };
        std::atomic<std::uint64_t> mReceived{ 0 };
        Queue mOutbound{ sOutboundCapacity };
        Queue mInbound;
        std::unique_ptr<Worker> mWorker;
    };

    std::shared_ptr<Connection> connect(const std::string& host, int port, std::size_t inboundCapacity);
}

#endif
