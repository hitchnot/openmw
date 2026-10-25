#include "connection.hpp"

#include "worker.hpp"

#include <stdexcept>
#include <utility>

namespace Ipc
{
    bool isLoopback(std::string_view host)
    {
        return host == "127.0.0.1" || host == "::1";
    }

    void validateEndpoint(std::string_view host, int port)
    {
        if (!isLoopback(host))
            throw std::invalid_argument("openmw.ipc: host must be 127.0.0.1 or ::1");
        if (port < 1 || port > 65535)
            throw std::invalid_argument("openmw.ipc: port must be an integer from 1 to 65535");
    }

    Connection::Connection(std::size_t inboundCapacity)
        : mInbound(inboundCapacity)
    {
    }

    Connection::~Connection()
    {
        close();
    }

    void Connection::start(const std::string& host, int port)
    {
        validateEndpoint(host, port);
        if (mWorker || state() != State::Connecting)
            throw std::logic_error("IPC connection already started or closed");
        mWorker = std::make_unique<Worker>(*this, host, port);
    }

    void Connection::opened()
    {
        State expected = State::Connecting;
        mState.compare_exchange_strong(expected, State::Open);
    }

    void Connection::finished()
    {
        mState = State::Closed;
        mOutbound.clear();
    }

    void Connection::close()
    {
        mState = State::Closed;
        mWorker.reset(); // Stop and join before destroying queues or dropping inbound data.
        mOutbound.clear();
        mInbound.clear();
    }

    bool Connection::send(std::string line)
    {
        if (line.find_first_of("\r\n") != std::string::npos)
            throw std::invalid_argument("openmw.ipc: line must not contain a newline");
        if (state() == State::Closed || !mOutbound.tryPush(std::move(line)))
            return false;
        if (state() == State::Closed)
        {
            mOutbound.clear();
            return false;
        }
        if (mWorker)
            mWorker->wake();
        return true;
    }

    void Connection::receive(std::string line)
    {
        ++mReceived;
        mInbound.pushDroppingOldest(std::move(line));
    }

    std::vector<std::string> Connection::drain(std::size_t max)
    {
        return drainFrame(mInbound, max);
    }

    std::shared_ptr<Connection> connect(const std::string& host, int port, std::size_t inboundCapacity)
    {
        auto connection = std::make_shared<Connection>(inboundCapacity);
        connection->start(host, port);
        return connection;
    }
}
