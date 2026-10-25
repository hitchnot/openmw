#include <apps/openmw/mwlua/context.hpp>
#include <apps/openmw/mwlua/corebindings.hpp>
#include <apps/openmw/mwlua/ipcbindings.hpp>

#include <components/ipc/connection.hpp>
#include <components/ipc/queue.hpp>
#include <components/lua/asyncpackage.hpp>
#include <components/lua/luastate.hpp>
#include <components/settings/values.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    TEST(dmoIpcQueue, OutboundRefusesFullAndKeepsAcceptedLinesInOrder)
    {
        Ipc::Connection connection;
        for (std::size_t i = 0; i < Ipc::sOutboundCapacity; ++i)
            ASSERT_TRUE(connection.send(std::to_string(i)));
        EXPECT_FALSE(connection.send("full"));
        EXPECT_EQ(connection.dropped(), 0);
        for (std::size_t i = 0; i < Ipc::sOutboundCapacity; ++i)
        {
            ASSERT_EQ(connection.takeOutbound(), std::to_string(i));
            connection.didSend();
        }
        EXPECT_FALSE(connection.takeOutbound());
        EXPECT_EQ(connection.sent(), Ipc::sOutboundCapacity);
        EXPECT_TRUE(connection.send("available"));
        EXPECT_EQ(connection.takeOutbound(), "available");
    }

    TEST(dmoIpcQueue, InboundOverflowDropsOldestAndCountsEveryDrop)
    {
        Ipc::Connection connection;
        for (int i = 0; i < 6000; ++i)
            connection.receive(std::to_string(i));
        EXPECT_EQ(connection.received(), 6000);
        EXPECT_EQ(connection.dropped(), 1904);
        int next = 1904;
        for (int frame = 0; frame < 16; ++frame)
        {
            const auto lines = connection.drain(10000);
            ASSERT_EQ(lines.size(), 256);
            for (const auto& line : lines)
                EXPECT_EQ(line, std::to_string(next++));
        }
        EXPECT_EQ(next, 6000);
        EXPECT_TRUE(connection.drain(256).empty());
        EXPECT_EQ(connection.dropped(), 1904);
    }

    TEST(dmoIpcQueue, DrainUsesCallerLimitAndFrameCapWithoutDroppingRemainder)
    {
        Ipc::Connection connection;
        for (int i = 0; i < 300; ++i)
            connection.receive(std::to_string(i));
        EXPECT_TRUE(connection.drain(0).empty());
        EXPECT_EQ(connection.drain(3), (std::vector<std::string>{ "0", "1", "2" }));
        EXPECT_EQ(connection.drain(1000).size(), Ipc::sDrainPerFrame);
        const auto tail = connection.drain(1000);
        ASSERT_EQ(tail.size(), 41);
        EXPECT_EQ(tail.front(), "259");
        EXPECT_EQ(tail.back(), "299");
        EXPECT_EQ(connection.dropped(), 0);
    }

    TEST(dmoIpcQueue, ConfiguredCapacityAndClearPreserveCounter)
    {
        EXPECT_THROW(Ipc::Queue(0), std::invalid_argument);
        Ipc::Queue queue(1);
        queue.pushDroppingOldest("old");
        queue.pushDroppingOldest("new");
        EXPECT_EQ(queue.dropped(), 1);
        EXPECT_EQ(queue.pop(), "new");
        EXPECT_TRUE(queue.tryPush("discard"));
        queue.clear();
        EXPECT_FALSE(queue.pop());
        EXPECT_EQ(queue.dropped(), 1);
    }

    TEST(dmoIpcQueue, LineSplitterHandlesFragmentedCoalescedAndCrLfLines)
    {
        Ipc::LineSplitter splitter;
        EXPECT_TRUE(splitter.append("{\"a\":").empty());
        EXPECT_EQ(splitter.append("1}\nsecond\r"), (std::vector<std::string>{ "{\"a\":1}" }));
        EXPECT_EQ(splitter.append("\n\nthird\nfour"), (std::vector<std::string>{ "second", "", "third" }));
        EXPECT_EQ(splitter.append("th\n"), (std::vector<std::string>{ "fourth" }));
        EXPECT_TRUE(splitter.append("unterminated").empty());
        Ipc::LineSplitter nextConnection;
        EXPECT_EQ(nextConnection.append("fresh\n"), (std::vector<std::string>{ "fresh" }));
    }

    TEST(dmoIpcQueue, LineSplitterPreservesLongLinesAndSplitUtf8Bytes)
    {
        Ipc::LineSplitter splitter;
        const std::string line(20000, 'x');
        for (std::size_t offset = 0; offset < line.size(); offset += 100)
            EXPECT_TRUE(splitter.append(std::string_view(line).substr(offset, 100)).empty());
        EXPECT_EQ(splitter.append("\n"), (std::vector<std::string>{ line }));
        EXPECT_TRUE(splitter.append("\xc3").empty());
        EXPECT_EQ(splitter.append("\xa9\n"), (std::vector<std::string>{ "\xc3\xa9" }));
    }

    TEST(dmoIpcQueue, LoopbackRequiresTheTwoLiteralAddressesAndValidPort)
    {
        for (const auto* host : { "127.0.0.1", "::1" })
        {
            EXPECT_TRUE(Ipc::isLoopback(host));
            EXPECT_NO_THROW(Ipc::validateEndpoint(host, 1));
            EXPECT_NO_THROW(Ipc::validateEndpoint(host, 65535));
        }
        for (const auto* host : { "localhost", "10.0.0.1", "127.0.0.2", "::ffff:127.0.0.1", "[::1]", "", "::" })
        {
            EXPECT_FALSE(Ipc::isLoopback(host));
            EXPECT_THROW(Ipc::validateEndpoint(host, 1234), std::invalid_argument);
        }
        for (int port : { -1, 0, 65536 })
            EXPECT_THROW(Ipc::validateEndpoint("127.0.0.1", port), std::invalid_argument);
    }

    TEST(dmoIpcQueue, StateAndCloseDiscardQueuesAndRejectFurtherSends)
    {
        Ipc::Connection connection;
        EXPECT_EQ(connection.state(), Ipc::State::Connecting);
        EXPECT_TRUE(connection.send("before open"));
        connection.opened();
        EXPECT_EQ(connection.state(), Ipc::State::Open);
        EXPECT_THROW(connection.send("two\nlines"), std::invalid_argument);
        EXPECT_THROW(connection.send("two\rlines"), std::invalid_argument);
        connection.receive("discard");
        connection.close();
        connection.close();
        connection.opened();
        EXPECT_EQ(connection.state(), Ipc::State::Closed);
        EXPECT_FALSE(connection.send("after close"));
        EXPECT_FALSE(connection.takeOutbound());
        EXPECT_TRUE(connection.drain(256).empty());
    }

    TEST(dmoIpcQueue, PeerDisconnectRetainsCompleteInboundLinesUntilExplicitClose)
    {
        Ipc::Connection connection;
        connection.opened();
        connection.receive("last line");
        connection.send("unsent");
        connection.finished();
        EXPECT_EQ(connection.state(), Ipc::State::Closed);
        EXPECT_FALSE(connection.takeOutbound());
        EXPECT_FALSE(connection.send("closed"));
        EXPECT_EQ(connection.drain(256), (std::vector<std::string>{ "last line" }));
    }

    TEST(dmoIpcQueue, ProducerAndConsumerKeepQueueOrderAcrossThreads)
    {
        Ipc::Queue queue(8);
        std::thread producer([&] {
            for (int i = 0; i < 1000; ++i)
                while (!queue.tryPush(std::to_string(i)))
                    std::this_thread::yield();
        });
        std::vector<std::string> lines;
        while (lines.size() < 1000)
        {
            if (auto line = queue.pop())
                lines.push_back(std::move(*line));
            else
                std::this_thread::yield();
        }
        producer.join();
        for (int i = 0; i < 1000; ++i)
            EXPECT_EQ(lines[i], std::to_string(i));
        EXPECT_EQ(queue.dropped(), 0);
    }

    class dmoIpcBindings : public testing::Test
    {
    protected:
        LuaUtil::LuaState mLua{ nullptr, nullptr };
        std::vector<std::shared_ptr<Ipc::Connection>> mTransports;
        MWLua::IpcManager mManager{ 512, [this](const std::string&, int, std::size_t capacity) {
            auto connection = std::make_shared<Ipc::Connection>(capacity);
            mTransports.push_back(connection);
            return connection;
        } };

        void SetUp() override
        {
            mLua.protectedCall([&](LuaUtil::LuaView& view) {
                auto lua = view.sol();
                lua["ipc"] = MWLua::initIpcPackage(lua, mManager);
                sol::table hidden(lua, sol::create);
                hidden[LuaUtil::ScriptsContainer::sScriptIdKey] = LuaUtil::ScriptId{};
                lua["hidden"] = hidden;
                auto meta = LuaUtil::Callback::makeMetatable(lua);
                lua["callback"] = [hidden, meta](sol::main_protected_function fn) {
                    return LuaUtil::Callback::make({ nullptr, -1, hidden }, std::move(fn), meta);
                };
            });
            run("c = ipc.connect{host='127.0.0.1', port=47805, name='a'}");
        }

        void TearDown() override
        {
            mLua.protectedCall([&](LuaUtil::LuaView&) { mManager.clear(); });
        }

        void run(std::string_view script)
        {
            std::string error;
            mLua.protectedCall([&](LuaUtil::LuaView& view) {
                auto result = view.sol().safe_script(script, sol::script_pass_on_error);
                if (!result.valid())
                    error = result.get<sol::error>().what();
            });
            ASSERT_TRUE(error.empty()) << error;
        }

        void update()
        {
            mLua.protectedCall([&](LuaUtil::LuaView&) { mManager.update(); });
        }
    };

    TEST_F(dmoIpcBindings, DuplicateNameIdentityStatesAndExplicitReconnection)
    {
        run(R"(
            assert(ipc.backend() == 'tcp' and c.state == 'connecting')
            assert(rawequal(c, ipc.connect{host='::1', port=1, name='a'}))
            assert(not pcall(function() c.state = 'open' end))
            assert(not pcall(function() ipc.backend = nil end))
        )");
        ASSERT_EQ(mTransports.size(), 1);
        mTransports[0]->opened();
        run("assert(c.state == 'open'); assert(c.stats.reconnects == 0)");
        mTransports[0]->finished();
        run(R"(
            assert(c.state == 'closed')
            next = ipc.connect{host='::1', port=47805, name='a'}
            assert(not rawequal(c, next) and c.state == 'closed' and next.state == 'connecting')
            assert(next.stats.reconnects == 1 and next.stats.sent == 0)
            next:close()
            again = ipc.connect{host='127.0.0.1', port=47805, name='a'}
            assert(again.stats.reconnects == 2)
        )");
        EXPECT_EQ(mTransports.size(), 3);
    }

    TEST_F(dmoIpcBindings, InvalidArgumentsAreRejectedBeforeCreatingTransport)
    {
        run(R"(
            for _, host in ipairs({'example.com', 'localhost', '10.0.0.1', '127.0.0.1\0extra'}) do
                assert(not pcall(ipc.connect, {host=host, port=1, name='a'}))
            end
            for _, port in ipairs({0, -1, 65536, 1.5, '1234', math.huge, 0/0}) do
                assert(not pcall(ipc.connect, {host='127.0.0.1', port=port, name='invalid'}))
            end
            assert(not pcall(ipc.connect, {host='::1', port=1}))
            assert(not pcall(function() c:poll(-1) end))
            assert(not pcall(function() c:poll(1.5) end))
            assert(not pcall(function() c:onMessage(function() end) end))
            assert(#c:poll(0) == 0)
        )");
        EXPECT_EQ(mTransports.size(), 1);
    }

    TEST_F(dmoIpcBindings, PollOnlySeesUpdateDeliveryAndStatsStayReadOnlyAndStable)
    {
        mTransports[0]->receive("one");
        mTransports[0]->receive("two");
        run("assert(#c:poll(10) == 0); stats = c.stats");
        update();
        run(R"(
            assert(rawequal(stats, c.stats) and stats.received == 2 and stats.dropped == 0)
            assert(not pcall(function() stats.received = 100 end))
            assert(c:poll(1)[1] == 'one')
            assert(c:poll(100)[1] == 'two')
            assert(#c:poll(100) == 0)
        )");
    }

    TEST_F(dmoIpcBindings, DrainCapIsSharedAndRotatesBetweenConnections)
    {
        run("b = ipc.connect{host='::1', port=1, name='b'}");
        for (auto& transport : mTransports)
            for (int i = 0; i < 300; ++i)
                transport->receive(std::to_string(i));
        update();
        run("assert(#c:poll(1000) == 256 and #b:poll(1000) == 0)");
        update();
        run("assert(#c:poll(1000) == 0 and #b:poll(1000) == 256)");
        update();
        run("assert(#c:poll(1000) == 44 and #b:poll(1000) == 44)");
    }

    TEST_F(dmoIpcBindings, UnpolledBufferIsBoundedAndAddsToDropCount)
    {
        for (int i = 0; i < 600; ++i)
            mTransports[0]->receive(std::to_string(i));
        update();
        update();
        for (int i = 600; i < 700; ++i)
            mTransports[0]->receive(std::to_string(i));
        update();
        run(R"(
            assert(c.stats.received == 700 and c.stats.dropped == 188)
            local lines = c:poll(1000)
            assert(#lines == 512 and lines[1] == '188' and lines[512] == '699')
        )");
    }

    TEST_F(dmoIpcBindings, CallbackRunsOnUpdateThreadAndNeverOnProducerThread)
    {
        const auto expected = std::this_thread::get_id();
        std::vector<std::thread::id> calls;
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["recordThread"] = [&] { calls.push_back(std::this_thread::get_id()); };
        });
        run("seen = {}; c:onMessage(callback(function(line) recordThread(); table.insert(seen, line) end))");
        std::thread producer([&] { mTransports[0]->receive("from transport"); });
        producer.join();
        EXPECT_TRUE(calls.empty());
        update();
        ASSERT_EQ(calls.size(), 1);
        EXPECT_EQ(calls[0], expected);
        run("assert(#seen == 1 and seen[1] == 'from transport' and #c:poll(1) == 0)");
        std::atomic_bool rejected{ false };
        std::thread wrongThread([&] {
            try
            {
                mManager.update();
            }
            catch (const std::logic_error&)
            {
                rejected = true;
            }
        });
        wrongThread.join();
        EXPECT_TRUE(rejected);
    }

    TEST_F(dmoIpcBindings, CallbackCanUnregisterAndReceivesBufferedLinesFirst)
    {
        mTransports[0]->receive("buffered");
        update();
        mTransports[0]->receive("new");
        run("seen = {}; c:onMessage(callback(function(line) table.insert(seen, line); c:onMessage(nil) end))");
        update();
        run("assert(#seen == 1 and seen[1] == 'buffered' and c:poll(1)[1] == 'new')");
        run("c:onMessage(callback(function() error('removed callback ran') end)); hidden._id = nil");
        mTransports[0]->receive("after removal");
        update();
        run("assert(c:poll(1)[1] == 'after removal')");
    }

    TEST_F(dmoIpcBindings, CallbackCanCloseAndReplaceItsOwnNameDuringDrain)
    {
        run(R"(
            calls = 0
            c:onMessage(callback(function()
                calls = calls + 1
                c:close()
                replacement = ipc.connect{host='::1', port=1, name='a'}
            end))
        )");
        mTransports[0]->receive("first");
        mTransports[0]->receive("discard");
        update();
        run("assert(calls == 1 and c.state == 'closed' and replacement.stats.reconnects == 1)");
        ASSERT_EQ(mTransports.size(), 2);
        mTransports[1]->receive("replacement");
        update();
        run("assert(replacement:poll(1)[1] == 'replacement' and #c:poll(10) == 0)");
    }

    TEST_F(dmoIpcBindings, ClearClosesEveryHandleDropsCallbacksAndResetsNames)
    {
        run(R"(
            b = ipc.connect{host='::1', port=1, name='b'}
            calls = 0
            c:onMessage(callback(function() calls = calls + 1 end))
        )");
        mTransports[0]->receive("discard");
        mTransports[1]->receive("discard");
        mLua.protectedCall([&](LuaUtil::LuaView&) { mManager.clear(); });
        update();
        for (auto& transport : mTransports)
            EXPECT_EQ(transport->state(), Ipc::State::Closed);
        run(R"(
            assert(c.state == 'closed' and b.state == 'closed' and calls == 0)
            assert(#c:poll(1) == 0 and #b:poll(1) == 0 and not c:send('closed'))
            assert(not pcall(function() c:onMessage(callback(function() end)) end))
            new = ipc.connect{host='::1', port=1, name='a'}
            assert(new.stats.reconnects == 0)
        )");
    }

    TEST_F(dmoIpcBindings, ExtensionsAdvertiseIpcBesideWatchAndRejectWrites)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            view.sol()["extensions"] = MWLua::initCoreApiExtensions(view.sol());
        });
        run(R"(
            assert(extensions.ipc == 1 and extensions.watch == 1)
            assert(not pcall(function() extensions.ipc = 2 end))
            assert(extensions.ipc == 1)
        )");
    }

    TEST_F(dmoIpcBindings, ClearReleasesCallbackCaptureAfterLuaCollection)
    {
        std::weak_ptr<int> captured;
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            auto token = std::make_shared<int>(1);
            captured = token;
            view.sol()["held"] = [token] { return *token; };
        });
        run("c:onMessage(callback(held)); held = nil");
        EXPECT_FALSE(captured.expired());
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            mManager.clear();
            for (int i = 0; i < 5; ++i)
                lua_gc(view.sol(), LUA_GCCOLLECT, 0);
        });
        EXPECT_TRUE(captured.expired());
    }

    TEST(dmoIpcSettings, DisabledSettingOmitsPackageWithoutConstructingLuaOrTransport)
    {
        ASSERT_FALSE(Settings::lua().mDmoIpcEnabled.get());
        EXPECT_EQ(Settings::lua().mDmoIpcMaxQueue.get(), 4096);
        std::map<std::string, sol::object> packages;
        MWLua::Context context{};
        MWLua::addIpcPackage(packages, context);
        EXPECT_EQ(packages.count("openmw.ipc"), 0);
    }
}
