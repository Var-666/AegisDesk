#include "agent/api/http_json.h"
#include "agent/api/http_server.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
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
constexpr auto kSimulatedHandlerWork = std::chrono::milliseconds(10);

class RunningHttpServer {
public:
    explicit RunningHttpServer(agent::HttpServer::RequestHandler handler)
        : server_(
              {
                  .bind_address = "127.0.0.1",
                  .port = 0,
              },
              std::move(handler))
        , port_(server_.Start()) {}

    RunningHttpServer(const RunningHttpServer&) = delete;
    RunningHttpServer& operator=(const RunningHttpServer&) = delete;

    ~RunningHttpServer() {
        try {
            Stop();
        } catch (...) {}
    }

    [[nodiscard]] unsigned short Port() const noexcept {
        return port_;
    }

    void Stop() {
        if (server_.State() != agent::HttpServerState::kStopped) {
            server_.RequestStop();
            server_.Wait();
        }
    }

private:
    agent::HttpServer server_;
    unsigned short port_{0};
};

agent::HttpResponse SendGetRequest(const unsigned short port, const std::string& target) {
    asio::io_context io_context;
    beast::tcp_stream stream(io_context);
    stream.expires_after(kClientTimeout);
    stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    agent::HttpRequest request{http::verb::get, target, 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::user_agent, "aegis-http-baseline-test");
    http::write(stream, request);

    beast::flat_buffer buffer;
    agent::HttpResponse response;
    http::read(stream, buffer, response);

    beast::error_code ignored_error;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored_error);
    return response;
}

struct BaselineResult {
    std::size_t client_count{0};
    std::size_t successful_requests{0};
    double total_milliseconds{0.0};
    double p95_milliseconds{0.0};
};

BaselineResult RunConcurrentBatch(const unsigned short port, const std::size_t client_count) {
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    std::size_t ready_clients = 0;
    bool start = false;

    std::atomic<std::size_t> successful_requests{0};
    std::vector<double> latencies(client_count, 0.0);
    std::vector<std::thread> clients;
    clients.reserve(client_count);

    for (std::size_t index = 0; index < client_count; ++index) {
        clients.emplace_back([&, index] {
            {
                std::unique_lock lock(gate_mutex);
                ++ready_clients;
                gate_condition.notify_all();
                gate_condition.wait(lock, [&] { return start; });
            }

            const auto request_started_at = std::chrono::steady_clock::now();
            try {
                const agent::HttpResponse response = SendGetRequest(port, "/baseline");
                if (response.result() == http::status::ok && response.body() == R"({"status":"ok"})") {
                    successful_requests.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception&) {
                // The success count and test assertion report client failures.
            }
            const auto request_finished_at = std::chrono::steady_clock::now();
            latencies[index] =
                std::chrono::duration<double, std::milli>(request_finished_at - request_started_at).count();
        });
    }

    {
        std::unique_lock lock(gate_mutex);
        gate_condition.wait(lock, [&] { return ready_clients == client_count; });
        start = true;
    }

    const auto batch_started_at = std::chrono::steady_clock::now();
    gate_condition.notify_all();

    for (std::thread& client : clients) {
        client.join();
    }

    const auto batch_finished_at = std::chrono::steady_clock::now();
    std::ranges::sort(latencies);
    const std::size_t p95_index = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(client_count))) - 1;

    return {
        .client_count = client_count,
        .successful_requests = successful_requests.load(std::memory_order_relaxed),
        .total_milliseconds = std::chrono::duration<double, std::milli>(batch_finished_at - batch_started_at).count(),
        .p95_milliseconds = latencies[p95_index],
    };
}

TEST(HttpServerIntegrationTest, BindsToEphemeralPortAndServesRequest) {
    RunningHttpServer server(
        [](const agent::HttpRequest&) { return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})"); });

    ASSERT_NE(server.Port(), 0);
    const agent::HttpResponse response = SendGetRequest(server.Port(), "/health");

    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_EQ(response.body(), R"({"status":"ok"})");

    server.Stop();
}

TEST(HttpServerLifecycleTest, SupportsExplicitStopAndRestartOnTheSameInstance) {
    agent::HttpServer server(
        {
            .bind_address = "127.0.0.1",
            .port = 0,
        },
        [](const agent::HttpRequest&) { return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})"); });

    EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    EXPECT_EQ(server.BoundPort(), 0);
    EXPECT_EQ(server.Options().io_thread_count, 1U);
    EXPECT_EQ(server.Options().handler_thread_count, 4U);

    const unsigned short first_port = server.Start();
    ASSERT_NE(first_port, 0);
    EXPECT_EQ(server.BoundPort(), first_port);
    EXPECT_EQ(server.State(), agent::HttpServerState::kRunning);
    EXPECT_THROW(static_cast<void>(server.Start()), std::logic_error);

    const agent::HttpResponse first_response = SendGetRequest(first_port, "/first");
    EXPECT_EQ(first_response.result(), http::status::ok);

    server.RequestStop();
    server.RequestStop();
    EXPECT_EQ(server.State(), agent::HttpServerState::kStopping);
    server.Wait();

    EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    EXPECT_EQ(server.BoundPort(), 0);

    const unsigned short second_port = server.Start();
    ASSERT_NE(second_port, 0);
    const agent::HttpResponse second_response = SendGetRequest(second_port, "/second");
    EXPECT_EQ(second_response.result(), http::status::ok);

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerLifecycleTest, RejectsInvalidConfigurationWithoutLeavingPartialState) {
    agent::HttpServerOptions options;
    options.port = 0;
    options.handler_thread_count = 0;

    agent::HttpServer server(
        options,
        [](const agent::HttpRequest&) { return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})"); });

    EXPECT_THROW(static_cast<void>(server.Start()), std::invalid_argument);
    EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    EXPECT_EQ(server.BoundPort(), 0);
}

TEST(HttpServerLifecycleTest, RunRemainsACompatibleBlockingEntryPoint) {
    agent::HttpServer server(
        {
            .bind_address = "127.0.0.1",
            .port = 0,
        },
        [](const agent::HttpRequest&) { return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})"); });

    std::atomic_bool stop_requested{false};
    std::exception_ptr run_error;
    std::thread runner([&] {
        try {
            server.Run([&] { return stop_requested.load(std::memory_order_acquire); });
        } catch (...) {
            run_error = std::current_exception();
        }
    });

    const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server.BoundPort() == 0 && std::chrono::steady_clock::now() < startup_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const unsigned short port = server.BoundPort();
    if (port != 0) {
        const agent::HttpResponse response = SendGetRequest(port, "/compatibility");
        EXPECT_EQ(response.result(), http::status::ok);
    }

    stop_requested.store(true, std::memory_order_release);
    runner.join();

    if (run_error != nullptr) {
        std::rethrow_exception(run_error);
    }

    EXPECT_NE(port, 0);
    EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    EXPECT_EQ(server.BoundPort(), 0);
}

TEST(HttpServerBaselineTest, RecordsSerialServerConcurrencyBaseline) {
    RunningHttpServer server([](const agent::HttpRequest&) {
        std::this_thread::sleep_for(kSimulatedHandlerWork);
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    for (const std::size_t client_count : {std::size_t{1}, std::size_t{10}, std::size_t{20}, std::size_t{50}}) {
        const BaselineResult result = RunConcurrentBatch(server.Port(), client_count);
        const double success_rate =
            100.0 * static_cast<double>(result.successful_requests) / static_cast<double>(result.client_count);

        std::cout << "[ HTTP BASELINE ] clients=" << result.client_count << " success_rate=" << success_rate
                  << "% total_ms=" << result.total_milliseconds << " p95_ms=" << result.p95_milliseconds << '\n';

        EXPECT_EQ(result.successful_requests, result.client_count);
    }

    server.Stop();
}

} // namespace
} // namespace aegis::test
