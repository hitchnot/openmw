#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <components/ipc/connection.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

#ifdef _WIN32
    using SocketHandle = SOCKET;
    using SocketLength = int;
    constexpr SocketHandle sInvalidSocket = INVALID_SOCKET;

    struct SocketRuntime
    {
        SocketRuntime()
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                throw std::runtime_error("test peer: WSAStartup failed");
        }
        ~SocketRuntime() { WSACleanup(); }
    };

    bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
    bool setNonblocking(SocketHandle socket)
    {
        u_long enabled = 1;
        return ioctlsocket(socket, FIONBIO, &enabled) == 0;
    }
    bool hasPendingBytes(SocketHandle socket)
    {
        u_long bytes = 0;
        if (ioctlsocket(socket, FIONREAD, &bytes) != 0)
            throw std::runtime_error("test peer: FIONREAD failed");
        return bytes > 0;
    }
#else
    using SocketHandle = int;
    using SocketLength = socklen_t;
    constexpr SocketHandle sInvalidSocket = -1;
    struct SocketRuntime
    {
    };

    bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR; }
    bool setNonblocking(SocketHandle socket)
    {
        const int flags = fcntl(socket, F_GETFL, 0);
        return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
    }
    bool hasPendingBytes(SocketHandle socket)
    {
        int bytes = 0;
        if (ioctl(socket, FIONREAD, &bytes) != 0)
            throw std::runtime_error("test peer: FIONREAD failed");
        return bytes > 0;
    }
#endif

    struct Socket
    {
        SocketHandle mHandle = sInvalidSocket;
        ~Socket()
        {
            if (mHandle == sInvalidSocket)
                return;
#ifdef _WIN32
            closesocket(mHandle);
#else
            ::close(mHandle);
#endif
        }
    };

    void configureSocket(SocketHandle socket)
    {
#ifndef _WIN32
        if (socket >= FD_SETSIZE)
            throw std::runtime_error("test peer: socket exceeds select capacity");
#endif
        if (!setNonblocking(socket))
            throw std::runtime_error("test peer: nonblocking setup failed");
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
            throw std::runtime_error("test peer: SO_NOSIGPIPE failed");
#endif
    }

    template <class Predicate>
    bool waitFor(Predicate predicate)
    {
        const auto deadline = Clock::now() + 2s;
        do
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for(1ms);
        } while (Clock::now() < deadline);
        return predicate();
    }

    class LoopbackPeer
    {
    public:
        // Zero lines means accept and hold the socket without ever reading it.
        explicit LoopbackPeer(std::size_t lines)
        {
            mListener.mHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (mListener.mHandle == sInvalidSocket)
                throw std::runtime_error("test peer: socket failed");
            configureSocket(mListener.mHandle);
            if (lines == 0)
            {
                const int size = 4096;
                if (setsockopt(mListener.mHandle, SOL_SOCKET, SO_RCVBUF,
                        reinterpret_cast<const char*>(&size), sizeof(size)) != 0)
                    throw std::runtime_error("test peer: SO_RCVBUF failed");
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            if (bind(mListener.mHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
                || listen(mListener.mHandle, 1) != 0)
                throw std::runtime_error("test peer: bind/listen failed");
            SocketLength size = sizeof(address);
            if (getsockname(mListener.mHandle, reinterpret_cast<sockaddr*>(&address), &size) != 0)
                throw std::runtime_error("test peer: getsockname failed");
            mPort = ntohs(address.sin_port);
            mThread = std::thread([this, lines] {
                try
                {
                    serve(lines);
                }
                catch (const std::exception& error)
                {
                    mError = error.what();
                }
                mFinished = true;
            });
        }

        ~LoopbackPeer() { stopAndJoin(); }
        int port() const { return mPort; }
        bool accepted() const { return mAccepted.load(); }
        bool hasBytes() const { return mHasBytes.load(); }
        bool finished() const { return mFinished.load(); }
        const std::vector<std::string>& lines() const { return mLines; } // Read only after join.
        const std::string& error() const { return mError; } // Read only after join.

        void stopAndJoin()
        {
            mStop = true;
            if (mThread.joinable())
                mThread.join();
        }

    private:
        bool waitSocket(SocketHandle socket, bool writing, Clock::time_point deadline)
        {
            while (!mStop && Clock::now() < deadline)
            {
                fd_set ready;
                FD_ZERO(&ready);
                FD_SET(socket, &ready);
                timeval timeout{ 0, 10000 };
#ifdef _WIN32
                const int nfds = 0;
#else
                const int nfds = socket + 1;
#endif
                const int result
                    = select(nfds, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &timeout);
                if (result > 0)
                    return true;
                if (result < 0)
                    throw std::runtime_error("test peer: select failed");
            }
            if (!mStop)
                throw std::runtime_error("test peer: socket deadline exceeded");
            return false;
        }

        bool writeLine(SocketHandle socket, const std::string& line, Clock::time_point deadline)
        {
            std::size_t offset = 0;
            while (offset < line.size() && waitSocket(socket, true, deadline))
            {
                const int size = static_cast<int>(std::min<std::size_t>(4096, line.size() - offset));
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL;
#endif
                const auto sent = send(socket, line.data() + offset, size, flags);
                if (sent > 0)
                    offset += static_cast<std::size_t>(sent);
                else if (sent == 0 || !wouldBlock())
                    throw std::runtime_error("test peer: send failed");
            }
            return offset == line.size();
        }

        void serve(std::size_t count)
        {
            const auto deadline = Clock::now() + 3s;
            if (!waitSocket(mListener.mHandle, false, deadline))
                return;
            Socket client{ accept(mListener.mHandle, nullptr, nullptr) };
            if (client.mHandle == sInvalidSocket)
                throw std::runtime_error("test peer: accept failed");
            configureSocket(client.mHandle);
            const int enabled = 1;
            if (setsockopt(client.mHandle, IPPROTO_TCP, TCP_NODELAY,
                    reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
                throw std::runtime_error("test peer: TCP_NODELAY failed");
            mAccepted = true;
            if (count == 0)
            {
                while (!mStop && Clock::now() < deadline)
                {
                    // Observe backpressure without consuming any of the client's data.
                    if (!mHasBytes && hasPendingBytes(client.mHandle))
                        mHasBytes = true;
                    std::this_thread::sleep_for(1ms);
                }
                if (!mStop)
                    throw std::runtime_error("test peer: hold deadline exceeded");
                return;
            }

            std::string pending;
            char buffer[2048];
            while (mLines.size() < count && waitSocket(client.mHandle, false, deadline))
            {
                const auto size = recv(client.mHandle, buffer, static_cast<int>(sizeof(buffer)), 0);
                if (size <= 0)
                {
                    if (size < 0 && wouldBlock())
                        continue;
                    throw std::runtime_error("test peer: disconnected before all lines arrived");
                }
                pending.append(buffer, static_cast<std::size_t>(size));
                // Deliberately independent of the production LineSplitter.
                for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n'))
                {
                    mLines.push_back(pending.substr(0, end));
                    if (!writeLine(client.mHandle, pending.substr(0, end + 1), deadline))
                        return;
                    pending.erase(0, end + 1);
                }
            }
            // Closing the peer here must make the real Worker report Closed.
        }

        SocketRuntime mRuntime;
        Socket mListener;
        int mPort = 0;
        std::atomic_bool mStop{ false }, mAccepted{ false }, mHasBytes{ false }, mFinished{ false };
        std::vector<std::string> mLines;
        std::string mError;
        std::thread mThread;
    };

    void runSocketTest(void (*test)())
    {
        // A broken production close/join cannot be cancelled safely in-process.
        // ASSERT_EXIT confines the watchdog's nonzero exit to this test's child.
        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock lock(mutex);
            if (!condition.wait_for(lock, 5s, [&] { return done; }))
            {
                std::cerr << "IPC socket test exceeded five seconds (including shutdown joins)" << std::endl;
                std::_Exit(124);
            }
        });
        try
        {
            test();
        }
        catch (const std::exception& error)
        {
            ADD_FAILURE() << error.what();
        }
        {
            const std::lock_guard lock(mutex);
            done = true;
        }
        condition.notify_one();
        watchdog.join();
        // Death-test children suppress gtest event listeners. Read their recorded
        // results directly so stderr retains each assertion, location and values.
        const auto* result = testing::UnitTest::GetInstance()->current_test_info()->result();
        for (int i = 0; i < result->total_part_count(); ++i)
        {
            const auto& part = result->GetTestPartResult(i);
            if (part.failed())
                std::cerr << (part.file_name() ? part.file_name() : "<unknown>") << ':' << part.line_number() << ": "
                          << part.message() << std::endl;
        }
        std::_Exit(testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    void echoAndDisconnect()
    {
        std::vector<std::string> expected;
        for (int i = 0; i < 400; ++i)
            expected.push_back("{\"seq\":" + std::to_string(i) + ",\"text\":\""
                + std::string(i == 199 ? 65536 : i % 100 * 23, static_cast<char>('a' + i % 26)) + "\xc3\xa9\"}");
        LoopbackPeer peer(expected.size());
        auto connection = Ipc::connect("127.0.0.1", peer.port(), Ipc::sInboundCapacity);
        // The factory starts asynchronously and may already be open before returning.
        const auto initial = connection->state();
        ASSERT_TRUE(initial == Ipc::State::Connecting || initial == Ipc::State::Open);
        ASSERT_TRUE(waitFor([&] { return connection->state() == Ipc::State::Open && peer.accepted(); }));
        for (const auto& line : expected)
            ASSERT_TRUE(connection->send(line));
        std::vector<std::string> received;
        ASSERT_TRUE(waitFor([&] {
            for (auto& line : connection->drain(Ipc::sDrainPerFrame))
                received.push_back(std::move(line));
            return received.size() >= expected.size();
        }));
        EXPECT_EQ(received, expected);
        ASSERT_TRUE(waitFor([&] { return connection->state() == Ipc::State::Closed && peer.finished(); }));
        peer.stopAndJoin();
        EXPECT_TRUE(peer.error().empty()) << peer.error();
        EXPECT_EQ(peer.lines(), expected);
        EXPECT_EQ(connection->sent(), expected.size());
        EXPECT_EQ(connection->received(), expected.size());
        EXPECT_EQ(connection->dropped(), 0);
        connection->close();
    }

    void connectingAndIdleClose()
    {
        LoopbackPeer peer(0);
        // Observe Connecting deterministically before starting the same Worker used by the factory.
        Ipc::Connection connection;
        ASSERT_EQ(connection.state(), Ipc::State::Connecting);
        connection.start("127.0.0.1", peer.port());
        ASSERT_TRUE(waitFor([&] { return connection.state() == Ipc::State::Open && peer.accepted(); }));
        const auto start = Clock::now();
        connection.close();
        EXPECT_LT(Clock::now() - start, 1s);
        EXPECT_EQ(connection.state(), Ipc::State::Closed);
        EXPECT_FALSE(peer.finished());
        peer.stopAndJoin();
        EXPECT_TRUE(peer.error().empty()) << peer.error();
    }

    void closeWithBackpressure()
    {
        LoopbackPeer peer(0);
        auto connection = Ipc::connect("127.0.0.1", peer.port(), Ipc::sInboundCapacity);
        const auto report = [&](const char* phase) {
            const auto state = connection->state();
            const char* stateName
                = state == Ipc::State::Connecting ? "connecting" : state == Ipc::State::Open ? "open" : "closed";
            std::cerr << "IPC backpressure " << phase << ": state=" << stateName
                      << " sent=" << connection->sent() << " received=" << connection->received()
                      << " dropped=" << connection->dropped() << " peer.accepted=" << peer.accepted()
                      << " peer.hasBytes=" << peer.hasBytes() << " peer.finished=" << peer.finished() << std::endl;
        };
        const bool opened = waitFor([&] { return connection->state() == Ipc::State::Open && peer.accepted(); });
        report("after connect wait");
        ASSERT_TRUE(opened);
        const std::string line(1024 * 1024, 'x');
        constexpr std::uint64_t maxLines = 512;
        std::uint64_t queued = 0;
        auto lastSent = connection->sent();
        auto lastProgress = Clock::now();
        const auto deadline = lastProgress + 2s;
        bool backpressure = false;
        while (Clock::now() < deadline)
        {
            ASSERT_EQ(connection->state(), Ipc::State::Open);
            ASSERT_FALSE(peer.finished());
            const auto sent = connection->sent();
            // Time with an empty outbound queue cannot count as a stalled writer.
            if (sent != lastSent || sent == queued)
            {
                lastSent = sent;
                lastProgress = Clock::now();
            }
            // Keep work pending without accumulating hundreds of MiB in the application queue.
            while (queued < maxLines && queued - sent < 4)
            {
                ASSERT_TRUE(connection->send(line));
                ++queued;
            }
            if (queued > sent && peer.hasBytes() && Clock::now() - lastProgress >= 200ms && connection->sent() == sent)
            {
                backpressure = true;
                break;
            }
            if (queued == maxLines && sent == queued)
                break;
            std::this_thread::sleep_for(1ms);
        }
        const auto stalled = std::chrono::duration<double, std::milli>(Clock::now() - lastProgress).count();
        report("after backpressure wait, before close");
        std::cerr << "IPC backpressure queued_lines=" << queued << " payload_bytes=" << queued * line.size()
                  << " last_sent=" << lastSent << " unchanged_ms=" << stalled << " reached=" << backpressure
                  << std::endl;
        ASSERT_TRUE(backpressure)
            << "Back-pressure was never reached within 512 MiB of payload and two seconds; queued=" << queued
            << " sent=" << connection->sent() << " unchanged_ms=" << stalled;
        ASSERT_FALSE(peer.finished());
        const auto start = Clock::now();
        connection->close();
        const auto elapsed = Clock::now() - start;
        std::cerr << "IPC backpressure close returned in " << std::chrono::duration<double, std::milli>(elapsed).count()
                  << " ms" << std::endl;
        report("after close returned");
        EXPECT_LT(elapsed, 1s);
        EXPECT_EQ(connection->state(), Ipc::State::Closed);
        EXPECT_FALSE(connection->send("closed"));
        EXPECT_FALSE(peer.finished());
        peer.stopAndJoin();
        std::cerr << "IPC backpressure peer joined; error=" << peer.error() << std::endl;
        EXPECT_TRUE(peer.error().empty()) << peer.error();
    }

    TEST(dmoIpcSocketDeathTest, RealFactoryEchoesIntactOrderedLinesAndObservesPeerDisconnect)
    {
        ASSERT_EXIT(runSocketTest(echoAndDisconnect), testing::ExitedWithCode(EXIT_SUCCESS), "");
    }

    TEST(dmoIpcSocketDeathTest, ConnectingBecomesOpenAndIdleCloseJoinsWhilePeerStaysConnected)
    {
        ASSERT_EXIT(runSocketTest(connectingAndIdleClose), testing::ExitedWithCode(EXIT_SUCCESS), "");
    }

    TEST(dmoIpcSocketDeathTest, RealFactoryCloseJoinsWithUnreadDataAndBlockedWriter)
    {
        ASSERT_EXIT(runSocketTest(closeWithBackpressure), testing::ExitedWithCode(EXIT_SUCCESS), "");
    }
}
