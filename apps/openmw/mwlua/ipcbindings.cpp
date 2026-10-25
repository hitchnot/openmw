#include "ipcbindings.hpp"

#include <components/lua/asyncpackage.hpp>
#include <components/lua/luastate.hpp>
#include <components/settings/values.hpp>

#include "context.hpp"
#include "luamanagerimp.hpp"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace MWLua
{
    class LuaIpcConnection
    {
    public:
        LuaIpcConnection(sol::state_view lua, std::shared_ptr<Ipc::Connection> transport, std::size_t capacity,
            std::uint64_t reconnects)
            : mTransport(std::move(transport))
            , mPolled(capacity)
            , mStats(lua, sol::create)
            , mReadOnlyStats(LuaUtil::makeReadOnly(mStats))
            , mReconnects(reconnects)
        {
        }

        const char* state() const
        {
            switch (mTransport->state())
            {
                case Ipc::State::Connecting:
                    return "connecting";
                case Ipc::State::Open:
                    return "open";
                case Ipc::State::Closed:
                    return "closed";
            }
            throw std::logic_error("Invalid IPC state");
        }

        sol::table stats()
        {
            mStats["sent"] = mTransport->sent();
            mStats["received"] = mTransport->received();
            mStats["dropped"] = mTransport->dropped() + mPolled.dropped();
            mStats["reconnects"] = mReconnects;
            return mReadOnlyStats;
        }

        bool send(std::string line) { return mTransport->send(std::move(line)); }

        sol::table poll(sol::this_state state, double max)
        {
            if (!std::isfinite(max) || max < 0 || std::floor(max) != max)
                throw std::invalid_argument("openmw.ipc: poll max must be a nonnegative integer");
            sol::table result(state, sol::create);
            // No conversion of an arbitrary Lua number to size_t is needed.
            while (max-- > 0)
            {
                auto line = mPolled.pop();
                if (!line)
                    break;
                result.add(*line);
            }
            return result;
        }

        void onMessage(const sol::object& callback)
        {
            if (callback == sol::nil)
                mCallback.reset();
            else
            {
                if (!LuaUtil::Callback::isLuaCallback(callback))
                    throw std::invalid_argument("openmw.ipc: onMessage expects an async callback or nil");
                if (mClosed)
                    throw std::logic_error("openmw.ipc: connection is closed");
                mCallback = LuaUtil::Callback::fromLua(callback.as<sol::table>());
            }
        }

        void close()
        {
            mClosed = true;
            mCallback.reset();
            mTransport->close();
            mPolled.clear();
        }

        std::size_t update(std::size_t max, std::thread::id updateThread)
        {
            assert(std::this_thread::get_id() == updateThread);
            if (mClosed)
                return 0;
            if (mCallback && !mCallback->isValid())
                mCallback.reset();
            // Keep buffered poll lines in order when switching to callbacks.
            std::size_t delivered = 0;
            while (delivered < max && mCallback && mCallback->isValid())
            {
                auto line = mPolled.pop();
                if (!line)
                    break;
                const auto callback = *mCallback; // A callback may replace itself or close this connection.
                callback.tryCall(*line);
                ++delivered;
                if (mClosed)
                    return delivered;
            }
            for (auto& line : mTransport->drain(max - delivered))
            {
                ++delivered;
                if (mCallback && !mCallback->isValid())
                    mCallback.reset();
                if (mCallback)
                {
                    assert(std::this_thread::get_id() == updateThread);
                    const auto callback = *mCallback;
                    callback.tryCall(line);
                }
                else
                    mPolled.pushDroppingOldest(std::move(line));
                if (mClosed)
                    break;
            }
            stats();
            return delivered;
        }

        bool isClosed() const { return mTransport->state() == Ipc::State::Closed; }
        std::uint64_t reconnects() const { return mReconnects; }

    private:
        std::shared_ptr<Ipc::Connection> mTransport;
        Ipc::Queue mPolled;
        sol::table mStats;
        sol::table mReadOnlyStats;
        std::uint64_t mReconnects;
        std::optional<LuaUtil::Callback> mCallback;
        bool mClosed = false;
    };

    IpcManager::IpcManager(std::size_t capacity, Factory factory)
        : mCapacity(capacity)
        , mFactory(std::move(factory))
    {
        if (capacity == 0)
            throw std::invalid_argument("IPC queue capacity must be positive");
    }

    IpcManager::~IpcManager()
    {
        clear();
    }

    sol::object IpcManager::connect(sol::state_view lua, const std::string& host, int port, const std::string& name)
    {
        Ipc::validateEndpoint(host, port);
        auto it = mConnections.find(name);
        std::uint64_t reconnects = 0;
        if (it != mConnections.end())
        {
            if (!it->second.mConnection->isClosed())
                return it->second.mUserdata;
            reconnects = it->second.mConnection->reconnects() + 1;
            it->second.mConnection->close();
        }
        auto connection
            = std::make_shared<LuaIpcConnection>(lua, mFactory(host, port, mCapacity), mCapacity, reconnects);
        sol::object userdata = sol::make_object(lua, connection);
        mConnections.insert_or_assign(name, Entry{ std::move(connection), userdata });
        return userdata;
    }

    void IpcManager::update()
    {
        const auto thread = std::this_thread::get_id();
        if (mUpdateThread && *mUpdateThread != thread)
            throw std::logic_error("IPC delivery must run on the Lua update thread");
        mUpdateThread = thread;
        // Callbacks can connect, replace a closed name, or close any connection.
        std::vector<std::shared_ptr<LuaIpcConnection>> connections;
        for (const auto& [name, entry] : mConnections)
            connections.push_back(entry.mConnection);
        std::size_t remaining = Ipc::sDrainPerFrame;
        for (std::size_t i = 0; i < connections.size() && remaining > 0; ++i)
            remaining -= connections[(mNext + i) % connections.size()]->update(remaining, thread);
        if (!connections.empty())
            mNext = (mNext + 1) % connections.size();
    }

    void IpcManager::clear()
    {
        for (auto& [name, entry] : mConnections)
            entry.mConnection->close();
        mConnections.clear();
        mUpdateThread.reset();
        mNext = 0;
    }

    sol::table initIpcPackage(sol::state_view lua, IpcManager& manager)
    {
        if (lua["IpcConnection"] == sol::nil)
        {
            auto connection = lua.new_usertype<LuaIpcConnection>("IpcConnection", sol::no_constructor);
            connection["state"] = sol::readonly_property(&LuaIpcConnection::state);
            connection["stats"] = sol::readonly_property(&LuaIpcConnection::stats);
            connection["send"] = &LuaIpcConnection::send;
            connection["poll"] = &LuaIpcConnection::poll;
            connection["onMessage"] = &LuaIpcConnection::onMessage;
            connection["close"] = &LuaIpcConnection::close;
        }
        sol::table api(lua, sol::create);
        api["connect"] = [lua, &manager](sol::table opts) {
            sol::object host = opts["host"], port = opts["port"], name = opts["name"];
            if (host.get_type() != sol::type::string || name.get_type() != sol::type::string)
                throw std::invalid_argument("openmw.ipc: host and name must be strings");
            if (port.get_type() != sol::type::number)
                throw std::invalid_argument("openmw.ipc: port must be an integer from 1 to 65535");
            const double number = port.as<double>();
            if (!std::isfinite(number) || number < 1 || number > 65535 || std::floor(number) != number)
                throw std::invalid_argument("openmw.ipc: port must be an integer from 1 to 65535");
            return manager.connect(lua, host.as<std::string>(), static_cast<int>(number), name.as<std::string>());
        };
        api["backend"] = [] { return "tcp"; };
        return LuaUtil::makeReadOnly(api);
    }

    void addIpcPackage(std::map<std::string, sol::object>& packages, const Context& context)
    {
        if (Settings::lua().mDmoIpcEnabled)
            packages.emplace("openmw.ipc", initIpcPackage(context.sol(), context.mLuaManager->ipc()));
    }
}
