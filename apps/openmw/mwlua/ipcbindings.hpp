#ifndef MWLUA_IPCBINDINGS_H
#define MWLUA_IPCBINDINGS_H

#include <components/ipc/connection.hpp>

#include <sol/sol.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace MWLua
{
    struct Context;
    class LuaIpcConnection;

    class IpcManager
    {
    public:
        using Factory = std::function<std::shared_ptr<Ipc::Connection>(const std::string&, int, std::size_t)>;
        explicit IpcManager(std::size_t capacity = Ipc::sInboundCapacity, Factory factory = Ipc::connect);
        ~IpcManager();
        sol::object connect(sol::state_view lua, const std::string& host, int port, const std::string& name);
        void update();
        void clear();

    private:
        struct Entry
        {
            std::shared_ptr<LuaIpcConnection> mConnection;
            sol::object mUserdata;
        };
        std::size_t mCapacity;
        Factory mFactory;
        std::map<std::string, Entry> mConnections;
        std::optional<std::thread::id> mUpdateThread;
        std::size_t mNext = 0;
    };

    sol::table initIpcPackage(sol::state_view lua, IpcManager& manager);
    void addIpcPackage(std::map<std::string, sol::object>& packages, const Context& context);
}

#endif
