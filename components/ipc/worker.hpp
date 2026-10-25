#ifndef OPENMW_COMPONENTS_IPC_WORKER_H
#define OPENMW_COMPONENTS_IPC_WORKER_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace Ipc
{
    class Connection;

    class Worker
    {
    public:
        Worker(Connection& connection, std::string host, int port);
        ~Worker();
        void wake();

    private:
        void run(const std::string& host, int port);
        Connection& mConnection;
        std::atomic_bool mStop{ false };
        std::mutex mMutex;
        std::condition_variable mCondition;
        bool mWake = false;
        std::thread mThread;
    };
}

#endif
