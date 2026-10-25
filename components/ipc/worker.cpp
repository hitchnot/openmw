#include "worker.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "connection.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace Ipc
{
    namespace
    {
#ifdef _WIN32
        using SocketHandle = SOCKET;
        constexpr SocketHandle invalidSocket = INVALID_SOCKET;
        using SocketLength = int;
        int socketError() { return WSAGetLastError(); }
        bool pending(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
        void closeSocket(SocketHandle socket) { closesocket(socket); }
        void shutdownSocket(SocketHandle socket) { shutdown(socket, SD_BOTH); }
        bool nonblocking(SocketHandle socket)
        {
            u_long mode = 1;
            return ioctlsocket(socket, FIONBIO, &mode) == 0;
        }
        struct SocketRuntime
        {
            WSADATA mData{};
            bool mReady = WSAStartup(MAKEWORD(2, 2), &mData) == 0;
            ~SocketRuntime()
            {
                if (mReady)
                    WSACleanup();
            }
        };
#else
        using SocketHandle = int;
        constexpr SocketHandle invalidSocket = -1;
        using SocketLength = socklen_t;
        int socketError() { return errno; }
        bool pending(int error) { return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS; }
        void closeSocket(SocketHandle socket) { ::close(socket); }
        void shutdownSocket(SocketHandle socket) { shutdown(socket, SHUT_RDWR); }
        bool nonblocking(SocketHandle socket)
        {
            const int flags = fcntl(socket, F_GETFL, 0);
            return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
        }
        struct SocketRuntime
        {
            bool mReady = true;
        };
#endif

        // Owned by the transport thread; shutdown wakes the reader before its join.
        struct Socket
        {
            SocketHandle mHandle = invalidSocket;
            ~Socket()
            {
                if (mHandle != invalidSocket)
                    closeSocket(mHandle);
            }
        };

        int waitSocket(SocketHandle socket, bool writing)
        {
            fd_set ready, errors;
            FD_ZERO(&ready);
            FD_ZERO(&errors);
            FD_SET(socket, &ready);
            FD_SET(socket, &errors);
            timeval timeout{ 0, 50000 };
#ifdef _WIN32
            const int nfds = 0;
#else
            const int nfds = socket + 1;
#endif
            return select(nfds, writing ? nullptr : &ready, writing ? &ready : nullptr, &errors, &timeout);
        }

        bool sendLine(SocketHandle socket, const std::string& line, const std::atomic_bool& stop)
        {
            std::size_t offset = 0;
            while (offset < line.size() && !stop)
            {
                const int ready = waitSocket(socket, true);
                if (ready < 0)
                    return false;
                if (ready == 0)
                    continue;
                const int size = static_cast<int>(std::min(line.size() - offset,
                    static_cast<std::size_t>(std::numeric_limits<int>::max())));
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL;
#endif
                const auto sent = ::send(socket, line.data() + offset, size, flags);
                if (sent > 0)
                    offset += static_cast<std::size_t>(sent);
                else if (sent == 0 || !pending(socketError()))
                    return false;
            }
            return offset == line.size();
        }
    }

    Worker::Worker(Connection& connection, std::string host, int port)
        : mConnection(connection)
        , mThread([this, host = std::move(host), port] {
            try
            {
                run(host, port);
            }
            catch (const std::exception&)
            {
                // Socket and thread setup failures are reported as a closed connection.
            }
            mConnection.finished();
        })
    {
    }

    Worker::~Worker()
    {
        mStop = true;
        wake();
        mThread.join();
    }

    void Worker::wake()
    {
        const std::lock_guard lock(mMutex);
        mWake = true;
        mCondition.notify_one();
    }

    void Worker::run(const std::string& host, int port)
    {
        SocketRuntime runtime;
        if (!runtime.mReady)
            return;
        const bool ipv6 = host == "::1";
        Socket socket{ ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP) };
        if (socket.mHandle == invalidSocket || !nonblocking(socket.mHandle))
            return;
#ifndef _WIN32
        if (socket.mHandle >= FD_SETSIZE)
            return;
#endif
        const int enabled = 1;
        const auto* option = reinterpret_cast<const char*>(&enabled);
        if (setsockopt(socket.mHandle, IPPROTO_TCP, TCP_NODELAY, option, sizeof(enabled)) != 0)
            return;
#ifdef SO_NOSIGPIPE
        setsockopt(socket.mHandle, SOL_SOCKET, SO_NOSIGPIPE, option, sizeof(enabled));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sockaddr_in6 address6{};
        address6.sin6_family = AF_INET6;
        address6.sin6_port = htons(static_cast<unsigned short>(port));
        address6.sin6_addr = in6addr_loopback;
        const auto* endpoint
            = ipv6 ? reinterpret_cast<const sockaddr*>(&address6) : reinterpret_cast<const sockaddr*>(&address);
        const auto length = static_cast<SocketLength>(ipv6 ? sizeof(address6) : sizeof(address));
        if (::connect(socket.mHandle, endpoint, length) != 0)
        {
            if (!pending(socketError()))
                return;
            int ready = 0;
            while (!mStop && (ready = waitSocket(socket.mHandle, true)) == 0)
            {
            }
            int error = 0;
            SocketLength errorSize = sizeof(error);
            if (mStop || ready < 0
                || getsockopt(socket.mHandle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorSize) != 0
                || error != 0)
                return;
        }
        if (mStop)
            return;
        mConnection.opened();
        std::thread reader([&] {
            try
            {
                LineSplitter splitter;
                char buffer[8192];
                while (!mStop)
                {
                    const int ready = waitSocket(socket.mHandle, false);
                    if (ready < 0)
                        break;
                    if (ready == 0)
                        continue;
                    const auto size = recv(socket.mHandle, buffer, static_cast<int>(sizeof(buffer)), 0);
                    if (size > 0)
                    {
                        for (auto& line : splitter.append(std::string_view(buffer, static_cast<std::size_t>(size))))
                            mConnection.receive(std::move(line));
                    }
                    else if (size == 0 || !pending(socketError()))
                        break;
                }
            }
            catch (const std::exception&)
            {
            }
            mStop = true;
            wake();
        });
        try
        {
            while (!mStop)
            {
                auto line = mConnection.takeOutbound();
                if (line)
                {
                    line->push_back('\n');
                    if (!sendLine(socket.mHandle, *line, mStop))
                        break;
                    mConnection.didSend();
                }
                else
                {
                    std::unique_lock lock(mMutex);
                    mCondition.wait(lock, [this] { return mStop || mWake; });
                    mWake = false;
                }
            }
        }
        catch (const std::exception&)
        {
        }
        mStop = true;
        shutdownSocket(socket.mHandle);
        reader.join();
    }
}
