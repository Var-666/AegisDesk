#include "agent/api/http_json.h"
#include "agent/api/http_server.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace aegis::test {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr auto kClientTimeout = std::chrono::seconds(5);
constexpr auto kConditionTimeout = std::chrono::seconds(3);

template <typename Predicate> bool WaitUntil(Predicate&& predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

agent::HttpResponse ExchangeRequest(beast::tcp_stream& stream, agent::HttpRequest request) {
    stream.expires_after(kClientTimeout);
    http::write(stream, request);

    beast::flat_buffer buffer;
    agent::HttpResponse response;
    stream.expires_after(kClientTimeout);
    http::read(stream, buffer, response);
    return response;
}

agent::HttpResponse SendGetRequest(const unsigned short port, const std::string& target) {
    asio::io_context io_context;
    beast::tcp_stream stream(io_context);
    stream.expires_after(kClientTimeout);
    stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    agent::HttpRequest request{http::verb::get, target, 11};
    request.set(http::field::host, "127.0.0.1");
    request.keep_alive(false);
    return ExchangeRequest(stream, std::move(request));
}

[[nodiscard]] std::optional<std::size_t> CountOpenFileDescriptors() {
#if defined(__linux__)
    const std::filesystem::path descriptor_directory{"/proc/self/fd"};
#elif defined(__APPLE__)
    const std::filesystem::path descriptor_directory{"/dev/fd"};
#else
    return std::nullopt;
#endif

    std::error_code error;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator iterator(descriptor_directory, error), end; !error && iterator != end;
         iterator.increment(error)) {
        ++count;
    }

    return error ? std::nullopt : std::optional<std::size_t>{count};
}

[[nodiscard]] bool SocketWasClosed(tcp::socket& socket) {
    std::array<char, 1> byte{};
    beast::error_code error;
    static_cast<void>(socket.read_some(asio::buffer(byte), error));

    if (!error || error == asio::error::would_block || error == asio::error::try_again) {
        return false;
    }

    return error == asio::error::eof || error == asio::error::connection_reset
           || error == asio::error::operation_aborted || error == asio::error::bad_descriptor;
}

TEST(HttpServerConcurrencyStressTest, MultipleKeepAliveClientsCompleteRepeatedRequests) {
    constexpr std::size_t kClientCount = 8;
    constexpr std::size_t kRequestsPerClient = 20;

    agent::HttpServerOptions options;
    options.port = 0;
    options.io_thread_count = 2;
    options.handler_thread_count = 4;
    options.max_connections = 16;
    options.max_in_flight_requests = 16;
    options.max_requests_per_connection = kRequestsPerClient;

    std::atomic_size_t handler_calls{0};
    agent::HttpServer server(options, [&](const agent::HttpRequest&) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });
    const unsigned short port = server.Start();

    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    std::size_t ready_clients = 0;
    bool start = false;
    std::atomic_size_t successful_requests{0};
    std::atomic_size_t client_errors{0};
    std::vector<std::thread> clients;
    clients.reserve(kClientCount);

    for (std::size_t client_index = 0; client_index < kClientCount; ++client_index) {
        clients.emplace_back([&, client_index] {
            {
                std::unique_lock lock(gate_mutex);
                ++ready_clients;
                gate_condition.notify_all();
                gate_condition.wait(lock, [&] { return start; });
            }

            try {
                asio::io_context io_context;
                beast::tcp_stream stream(io_context);
                stream.expires_after(kClientTimeout);
                stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

                for (std::size_t request_index = 0; request_index < kRequestsPerClient; ++request_index) {
                    agent::HttpRequest request{
                        http::verb::get,
                        "/keep-alive/" + std::to_string(client_index) + "/" + std::to_string(request_index), 11};
                    request.set(http::field::host, "127.0.0.1");
                    request.keep_alive(true);

                    const agent::HttpResponse response = ExchangeRequest(stream, std::move(request));
                    const bool is_last_request = request_index + 1 == kRequestsPerClient;
                    if (response.result() != http::status::ok || response.keep_alive() == is_last_request) {
                        client_errors.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    successful_requests.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception&) {
                client_errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    {
        std::unique_lock lock(gate_mutex);
        gate_condition.wait(lock, [&] { return ready_clients == kClientCount; });
        start = true;
    }
    gate_condition.notify_all();

    for (std::thread& client : clients) {
        client.join();
    }

    EXPECT_EQ(client_errors.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(successful_requests.load(std::memory_order_relaxed), kClientCount * kRequestsPerClient);
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), kClientCount * kRequestsPerClient);
    EXPECT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 0; }, kConditionTimeout));

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerConcurrencyStressTest, ConcurrentTrafficAndRepeatedStopRequestsDoNotDeadlock) {
    constexpr std::size_t kClientCount = 32;
    constexpr std::size_t kStopperCount = 8;

    agent::HttpServerOptions options;
    options.port = 0;
    options.io_thread_count = 4;
    options.handler_thread_count = 4;
    options.max_connections = 64;
    options.max_in_flight_requests = 64;
    options.shutdown_grace_period = std::chrono::seconds(2);

    std::atomic_size_t handler_calls{0};
    agent::HttpServer server(options, [&](const agent::HttpRequest&) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });
    const unsigned short port = server.Start();

    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool start = false;
    std::atomic_size_t client_outcomes{0};
    std::vector<std::thread> clients;
    clients.reserve(kClientCount);

    for (std::size_t index = 0; index < kClientCount; ++index) {
        clients.emplace_back([&] {
            {
                std::unique_lock lock(gate_mutex);
                gate_condition.wait(lock, [&] { return start; });
            }

            try {
                static_cast<void>(SendGetRequest(port, "/during-stop"));
            } catch (const std::exception&) {}
            client_outcomes.fetch_add(1, std::memory_order_relaxed);
        });
    }

    {
        std::scoped_lock lock(gate_mutex);
        start = true;
    }
    gate_condition.notify_all();
    EXPECT_TRUE(WaitUntil([&] { return handler_calls.load(std::memory_order_relaxed) > 0; }, kConditionTimeout));

    std::vector<std::thread> stoppers;
    stoppers.reserve(kStopperCount);
    const auto stop_started_at = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < kStopperCount; ++index) {
        stoppers.emplace_back([&] {
            for (std::size_t attempt = 0; attempt < 10; ++attempt) {
                server.RequestStop();
            }
        });
    }

    for (std::thread& stopper : stoppers) {
        stopper.join();
    }
    for (std::thread& client : clients) {
        client.join();
    }
    server.Wait();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started_at;

    EXPECT_EQ(client_outcomes.load(std::memory_order_relaxed), kClientCount);
    EXPECT_GT(handler_calls.load(std::memory_order_relaxed), 0U);
    EXPECT_LT(stop_elapsed, std::chrono::seconds(3));
    EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    EXPECT_EQ(server.ActiveSessionCount(), 0U);
    EXPECT_EQ(server.InFlightRequestCount(), 0U);
}

TEST(HttpServerFaultTest, PartialRequestTimesOutAndServerRemainsUsable) {
    agent::HttpServerOptions options;
    options.port = 0;
    options.read_timeout = std::chrono::seconds(1);

    agent::HttpServer server(options, [](const agent::HttpRequest&) {
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });
    const unsigned short port = server.Start();

    asio::io_context io_context;
    tcp::socket partial_client(io_context);
    partial_client.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    const std::string partial_request = "GET /partial HTTP/1.1\r\nHost:";
    asio::write(partial_client, asio::buffer(partial_request));

    ASSERT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 1; }, kConditionTimeout));
    EXPECT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 0; }, std::chrono::milliseconds(2500)));

    const agent::HttpResponse response = SendGetRequest(port, "/after-partial-timeout");
    EXPECT_EQ(response.result(), http::status::ok);

    beast::error_code ignored_error;
    partial_client.close(ignored_error);
    server.RequestStop();
    server.Wait();
}

TEST(HttpServerFaultTest, ClientDisconnectDuringHandlerDoesNotPoisonServer) {
    std::mutex handler_mutex;
    std::condition_variable handler_condition;
    bool release_handler = false;
    std::atomic_bool handler_started{false};

    agent::HttpServerOptions options;
    options.port = 0;
    agent::HttpServer server(options, [&](const agent::HttpRequest& request) {
        if (request.target() == "/disconnect") {
            std::unique_lock lock(handler_mutex);
            handler_started.store(true, std::memory_order_release);
            handler_condition.notify_all();
            handler_condition.wait(lock, [&] { return release_handler; });
        }
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });
    const unsigned short port = server.Start();

    asio::io_context io_context;
    tcp::socket disconnecting_client(io_context);
    disconnecting_client.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    agent::HttpRequest request{http::verb::get, "/disconnect", 11};
    request.set(http::field::host, "127.0.0.1");
    request.keep_alive(false);
    http::write(disconnecting_client, request);
    beast::error_code ignored_error;
    disconnecting_client.close(ignored_error);

    ASSERT_TRUE(WaitUntil([&] { return handler_started.load(std::memory_order_acquire); }, kConditionTimeout));
    {
        std::scoped_lock lock(handler_mutex);
        release_handler = true;
    }
    handler_condition.notify_all();

    EXPECT_TRUE(WaitUntil([&] { return server.InFlightRequestCount() == 0; }, kConditionTimeout));
    EXPECT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 0; }, kConditionTimeout));

    const agent::HttpResponse response = SendGetRequest(port, "/after-disconnect");
    EXPECT_EQ(response.result(), http::status::ok);

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerResourceTest, ConnectionLimitRejectsExcessClientAndRecoversCapacity) {
    agent::HttpServerOptions options;
    options.port = 0;
    options.max_connections = 2;
    options.max_in_flight_requests = 2;

    agent::HttpServer server(options, [](const agent::HttpRequest&) {
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });
    const unsigned short port = server.Start();
    const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);

    asio::io_context io_context;
    tcp::socket first_client(io_context);
    tcp::socket second_client(io_context);
    tcp::socket rejected_client(io_context);
    first_client.connect(endpoint);
    second_client.connect(endpoint);
    ASSERT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 2; }, kConditionTimeout));

    rejected_client.connect(endpoint);
    rejected_client.non_blocking(true);
    EXPECT_TRUE(WaitUntil([&] { return SocketWasClosed(rejected_client); }, kConditionTimeout));
    EXPECT_EQ(server.ActiveSessionCount(), 2U);

    beast::error_code ignored_error;
    first_client.close(ignored_error);
    ASSERT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 1; }, kConditionTimeout));

    tcp::socket replacement_client(io_context);
    replacement_client.connect(endpoint);
    EXPECT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 2; }, kConditionTimeout));

    second_client.close(ignored_error);
    rejected_client.close(ignored_error);
    replacement_client.close(ignored_error);
    server.RequestStop();
    server.Wait();
}

TEST(HttpServerResourceTest, RepeatedLifecycleDoesNotLeakFileDescriptors) {
    const std::optional<std::size_t> descriptors_before = CountOpenFileDescriptors();
    if (!descriptors_before.has_value()) {
        GTEST_SKIP() << "open file descriptor counting is unavailable on this platform";
    }

    {
        agent::HttpServerOptions options;
        options.port = 0;
        options.io_thread_count = 1;
        options.handler_thread_count = 1;
        options.max_connections = 8;
        options.max_in_flight_requests = 4;

        agent::HttpServer server(options, [](const agent::HttpRequest&) {
            return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
        });

        for (std::size_t cycle = 0; cycle < 50; ++cycle) {
            const unsigned short port = server.Start();
            const agent::HttpResponse response = SendGetRequest(port, "/lifecycle/" + std::to_string(cycle));
            ASSERT_EQ(response.result(), http::status::ok);

            server.RequestStop();
            server.Wait();
            ASSERT_EQ(server.State(), agent::HttpServerState::kStopped);
            ASSERT_EQ(server.ActiveSessionCount(), 0U);
            ASSERT_EQ(server.InFlightRequestCount(), 0U);
        }
    }

    const std::optional<std::size_t> descriptors_after = CountOpenFileDescriptors();
    ASSERT_TRUE(descriptors_after.has_value());
    EXPECT_LE(*descriptors_after, *descriptors_before + 2);
}

} // namespace
} // namespace aegis::test
