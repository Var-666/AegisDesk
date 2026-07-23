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
#include <optional>
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
constexpr auto kConditionTimeout = std::chrono::seconds(3);

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

agent::HttpResponse ExchangeRequest(beast::tcp_stream& stream, agent::HttpRequest request) {
    http::write(stream, request);

    beast::flat_buffer buffer;
    agent::HttpResponse response;
    http::read(stream, buffer, response);
    return response;
}

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

void UpdateMaximum(std::atomic_size_t& maximum, const std::size_t candidate) {
    std::size_t observed = maximum.load(std::memory_order_relaxed);
    while (
        candidate > observed
        && !maximum.compare_exchange_weak(observed, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
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

    agent::HttpServer server(options, [](const agent::HttpRequest&) {
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    EXPECT_THROW(static_cast<void>(server.Start()), std::invalid_argument);
    EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    EXPECT_EQ(server.BoundPort(), 0);
}

TEST(HttpServerLifecycleTest, RejectsInvalidProtocolAndShutdownLimits) {
    const auto handler = [](const agent::HttpRequest&) {
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    };

    {
        agent::HttpServerOptions options;
        options.port = 0;
        options.max_header_bytes = 0;
        agent::HttpServer server(options, handler);

        EXPECT_THROW(static_cast<void>(server.Start()), std::invalid_argument);
        EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    }

    {
        agent::HttpServerOptions options;
        options.port = 0;
        options.shutdown_grace_period = std::chrono::milliseconds::zero();
        agent::HttpServer server(options, handler);

        EXPECT_THROW(static_cast<void>(server.Start()), std::invalid_argument);
        EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    }
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

TEST(HttpServerAsyncSessionTest, IdleClientDoesNotBlockACompleteRequestOnOneIoThread) {
    agent::HttpServerOptions options;
    options.port = 0;
    options.io_thread_count = 1;
    options.read_timeout = std::chrono::seconds(5);

    agent::HttpServer server(options, [](const agent::HttpRequest&) {
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });
    const unsigned short port = server.Start();

    asio::io_context idle_client_context;
    tcp::socket idle_client(idle_client_context);
    idle_client.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    ASSERT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 1; }, kConditionTimeout));

    const auto request_started_at = std::chrono::steady_clock::now();
    const agent::HttpResponse response = SendGetRequest(port, "/while-idle-client-is-connected");
    const auto request_elapsed = std::chrono::steady_clock::now() - request_started_at;

    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_LT(request_elapsed, std::chrono::seconds(1));

    const auto stop_started_at = std::chrono::steady_clock::now();
    server.RequestStop();
    server.Wait();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started_at;

    EXPECT_LT(stop_elapsed, std::chrono::seconds(1));
    EXPECT_EQ(server.ActiveSessionCount(), 0U);

    beast::error_code ignored_error;
    idle_client.close(ignored_error);
}

TEST(HttpServerProtocolTest, EnforcesHeaderAndBodyLimits) {
    std::atomic_size_t handler_calls{0};

    {
        agent::HttpServerOptions options;
        options.port = 0;
        options.max_header_bytes = 128;

        agent::HttpServer server(options, [&](const agent::HttpRequest&) {
            handler_calls.fetch_add(1, std::memory_order_relaxed);
            return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
        });

        const unsigned short port = server.Start();
        asio::io_context io_context;
        beast::tcp_stream stream(io_context);
        stream.expires_after(kClientTimeout);
        stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

        agent::HttpRequest request{http::verb::get, "/large-header", 11};
        request.set(http::field::host, "127.0.0.1");
        request.set("X-Oversized-Header", std::string(512, 'x'));
        const agent::HttpResponse response = ExchangeRequest(stream, std::move(request));

        EXPECT_EQ(response.result(), http::status::request_header_fields_too_large);
        EXPECT_NE(response.body().find("header_too_large"), std::string::npos);

        server.RequestStop();
        server.Wait();
    }

    {
        agent::HttpServerOptions options;
        options.port = 0;
        options.max_body_bytes = 8;

        agent::HttpServer server(options, [&](const agent::HttpRequest&) {
            handler_calls.fetch_add(1, std::memory_order_relaxed);
            return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
        });

        const unsigned short port = server.Start();
        asio::io_context io_context;
        beast::tcp_stream stream(io_context);
        stream.expires_after(kClientTimeout);
        stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

        agent::HttpRequest request{http::verb::post, "/large-body", 11};
        request.set(http::field::host, "127.0.0.1");
        request.body() = std::string(64, 'b');
        request.prepare_payload();
        const agent::HttpResponse response = ExchangeRequest(stream, std::move(request));

        EXPECT_EQ(response.result(), http::status::payload_too_large);
        EXPECT_NE(response.body().find("body_too_large"), std::string::npos);

        server.RequestStop();
        server.Wait();
    }

    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 0U);
}

TEST(HttpServerProtocolTest, MalformedRequestReturnsBadRequest) {
    agent::HttpServerOptions options;
    options.port = 0;

    agent::HttpServer server(options, [](const agent::HttpRequest&) {
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    const unsigned short port = server.Start();
    asio::io_context io_context;
    beast::tcp_stream stream(io_context);
    stream.expires_after(kClientTimeout);
    stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    const std::string malformed_request = "GET / HTTP/1.1\r\nInvalid Header\r\n\r\n";
    asio::write(stream.socket(), asio::buffer(malformed_request));

    beast::flat_buffer buffer;
    agent::HttpResponse response;
    http::read(stream, buffer, response);

    EXPECT_EQ(response.result(), http::status::bad_request);
    EXPECT_NE(response.body().find("malformed_request"), std::string::npos);

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerProtocolTest, ReusesConnectionUntilConfiguredRequestLimit) {
    agent::HttpServerOptions options;
    options.port = 0;
    options.max_requests_per_connection = 2;

    std::atomic_size_t handler_calls{0};
    agent::HttpServer server(options, [&](const agent::HttpRequest&) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    const unsigned short port = server.Start();
    asio::io_context io_context;
    beast::tcp_stream stream(io_context);
    stream.expires_after(kClientTimeout);
    stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    agent::HttpRequest first_request{http::verb::get, "/first", 11};
    first_request.set(http::field::host, "127.0.0.1");
    first_request.keep_alive(true);
    const agent::HttpResponse first_response = ExchangeRequest(stream, std::move(first_request));

    agent::HttpRequest second_request{http::verb::get, "/second", 11};
    second_request.set(http::field::host, "127.0.0.1");
    second_request.keep_alive(true);
    const agent::HttpResponse second_response = ExchangeRequest(stream, std::move(second_request));

    EXPECT_EQ(first_response.result(), http::status::ok);
    EXPECT_TRUE(first_response.keep_alive());
    EXPECT_EQ(second_response.result(), http::status::ok);
    EXPECT_FALSE(second_response.keep_alive());
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 2U);
    EXPECT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 0; }, kConditionTimeout));

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerProtocolTest, ClosesIdleKeepAliveConnectionAfterIdleTimeout) {
    agent::HttpServerOptions options;
    options.port = 0;
    options.idle_timeout = std::chrono::seconds(1);

    agent::HttpServer server(options, [](const agent::HttpRequest&) {
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    const unsigned short port = server.Start();
    asio::io_context io_context;
    beast::tcp_stream stream(io_context);
    stream.expires_after(kClientTimeout);
    stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

    agent::HttpRequest request{http::verb::get, "/idle", 11};
    request.set(http::field::host, "127.0.0.1");
    request.keep_alive(true);
    const agent::HttpResponse response = ExchangeRequest(stream, std::move(request));

    ASSERT_TRUE(response.keep_alive());
    EXPECT_TRUE(WaitUntil([&] { return server.ActiveSessionCount() == 0; }, std::chrono::milliseconds(2500)));

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerBusinessIsolationTest, ConfiguredHandlerWorkersExecuteConcurrentlyWithOneIoWorker) {
    std::atomic_size_t active_handlers{0};
    std::atomic_size_t maximum_active_handlers{0};

    agent::HttpServerOptions options;
    options.port = 0;
    options.io_thread_count = 1;
    options.handler_thread_count = 4;
    options.max_in_flight_requests = 8;

    agent::HttpServer server(options, [&](const agent::HttpRequest&) {
        const std::size_t active = active_handlers.fetch_add(1, std::memory_order_acq_rel) + 1;
        UpdateMaximum(maximum_active_handlers, active);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        active_handlers.fetch_sub(1, std::memory_order_acq_rel);
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    const unsigned short port = server.Start();
    const BaselineResult result = RunConcurrentBatch(port, 8);

    std::cout << "[ HTTP HANDLERS ] clients=" << result.client_count << " success_rate="
              << (100.0 * static_cast<double>(result.successful_requests) / static_cast<double>(result.client_count))
              << "% total_ms=" << result.total_milliseconds << " p95_ms=" << result.p95_milliseconds
              << " max_active_handlers=" << maximum_active_handlers.load(std::memory_order_relaxed) << '\n';

    EXPECT_EQ(result.successful_requests, 8U);
    EXPECT_GE(maximum_active_handlers.load(std::memory_order_relaxed), 2U);
    EXPECT_LE(maximum_active_handlers.load(std::memory_order_relaxed), options.handler_thread_count);

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerBusinessIsolationTest, HandlerExceptionReturnsErrorAndDoesNotTerminateWorker) {
    std::atomic_size_t handler_calls{0};

    agent::HttpServerOptions options;
    options.port = 0;
    options.io_thread_count = 1;
    options.handler_thread_count = 1;

    agent::HttpServer server(options, [&](const agent::HttpRequest&) {
        if (handler_calls.fetch_add(1, std::memory_order_relaxed) == 0) {
            throw std::runtime_error("simulated handler failure");
        }
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    const unsigned short port = server.Start();
    const agent::HttpResponse failed_response = SendGetRequest(port, "/handler-failure");
    const agent::HttpResponse successful_response = SendGetRequest(port, "/after-handler-failure");

    EXPECT_EQ(failed_response.result(), http::status::internal_server_error);
    EXPECT_NE(failed_response.body().find("simulated handler failure"), std::string::npos);
    EXPECT_EQ(successful_response.result(), http::status::ok);
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 2U);

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerBackpressureTest, SaturatedExecutorReturnsServiceUnavailableWithoutBlockingIo) {
    std::mutex handler_mutex;
    std::condition_variable handler_condition;
    bool release_handler = false;
    std::atomic_bool handler_started{false};
    std::atomic_size_t handler_calls{0};

    agent::HttpServerOptions options;
    options.port = 0;
    options.io_thread_count = 1;
    options.handler_thread_count = 1;
    options.max_connections = 8;
    options.max_in_flight_requests = 1;

    agent::HttpServer server(options, [&](const agent::HttpRequest&) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock lock(handler_mutex);
            handler_started.store(true, std::memory_order_release);
            handler_condition.notify_all();
            handler_condition.wait(lock, [&] { return release_handler; });
        }
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    const unsigned short port = server.Start();
    std::optional<agent::HttpResponse> first_response;
    std::exception_ptr first_request_error;
    std::thread first_client([&] {
        try {
            first_response = SendGetRequest(port, "/slow");
        } catch (...) {
            first_request_error = std::current_exception();
        }
    });

    const bool started = WaitUntil([&] { return handler_started.load(std::memory_order_acquire); }, kConditionTimeout);
    if (!started) {
        {
            std::scoped_lock lock(handler_mutex);
            release_handler = true;
        }
        handler_condition.notify_all();
        first_client.join();
        server.RequestStop();
        server.Wait();
        FAIL() << "the first handler did not start";
        return;
    }

    const auto overload_started_at = std::chrono::steady_clock::now();
    const agent::HttpResponse overload_response = SendGetRequest(port, "/overload");
    const auto overload_elapsed = std::chrono::steady_clock::now() - overload_started_at;

    EXPECT_EQ(overload_response.result(), http::status::service_unavailable);
    EXPECT_EQ(overload_response[http::field::retry_after], "1");
    EXPECT_NE(overload_response.body().find("server_overloaded"), std::string::npos);
    EXPECT_LT(overload_elapsed, std::chrono::seconds(1));
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 1U);

    {
        std::scoped_lock lock(handler_mutex);
        release_handler = true;
    }
    handler_condition.notify_all();
    first_client.join();

    EXPECT_EQ(first_request_error, nullptr);
    ASSERT_TRUE(first_response.has_value());
    EXPECT_EQ(first_response->result(), http::status::ok);

    server.RequestStop();
    server.Wait();
}

TEST(HttpServerGracefulShutdownTest, DrainsQueuedWorkAndWaitsForRunningHandlers) {
    std::mutex handler_mutex;
    std::condition_variable handler_condition;
    bool release_handler = false;
    std::atomic_bool handler_started{false};
    std::atomic_size_t handler_calls{0};

    agent::HttpServerOptions options;
    options.port = 0;
    options.io_thread_count = 1;
    options.handler_thread_count = 1;
    options.max_connections = 8;
    options.max_in_flight_requests = 2;

    agent::HttpServer server(options, [&](const agent::HttpRequest&) {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock lock(handler_mutex);
            handler_started.store(true, std::memory_order_release);
            handler_condition.notify_all();
            handler_condition.wait(lock, [&] { return release_handler; });
        }
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    const unsigned short port = server.Start();
    std::optional<agent::HttpResponse> first_response;
    std::optional<agent::HttpResponse> queued_response;
    std::exception_ptr first_request_error;
    std::exception_ptr queued_request_error;

    std::thread first_client([&] {
        try {
            first_response = SendGetRequest(port, "/running");
        } catch (...) {
            first_request_error = std::current_exception();
        }
    });

    const bool started = WaitUntil([&] { return handler_started.load(std::memory_order_acquire); }, kConditionTimeout);
    if (!started) {
        {
            std::scoped_lock lock(handler_mutex);
            release_handler = true;
        }
        handler_condition.notify_all();
        first_client.join();
        server.RequestStop();
        server.Wait();
        FAIL() << "the running handler did not start";
        return;
    }

    std::thread queued_client([&] {
        try {
            queued_response = SendGetRequest(port, "/queued");
        } catch (...) {
            queued_request_error = std::current_exception();
        }
    });

    const bool queued = WaitUntil([&] { return server.InFlightRequestCount() == 2; }, kConditionTimeout);
    if (!queued) {
        {
            std::scoped_lock lock(handler_mutex);
            release_handler = true;
        }
        handler_condition.notify_all();
        first_client.join();
        queued_client.join();
        server.RequestStop();
        server.Wait();
        FAIL() << "the second request was not queued";
        return;
    }

    server.RequestStop();
    std::atomic_bool wait_completed{false};
    std::thread waiter([&] {
        server.Wait();
        wait_completed.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(wait_completed.load(std::memory_order_acquire));
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(server.InFlightRequestCount(), 2U);

    {
        std::scoped_lock lock(handler_mutex);
        release_handler = true;
    }
    handler_condition.notify_all();

    first_client.join();
    queued_client.join();
    waiter.join();

    EXPECT_TRUE(wait_completed.load(std::memory_order_acquire));
    EXPECT_EQ(handler_calls.load(std::memory_order_relaxed), 2U);
    EXPECT_EQ(server.InFlightRequestCount(), 0U);
    EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
    EXPECT_EQ(first_request_error, nullptr);
    EXPECT_EQ(queued_request_error, nullptr);
    ASSERT_TRUE(first_response.has_value());
    ASSERT_TRUE(queued_response.has_value());
    EXPECT_EQ(first_response->result(), http::status::ok);
    EXPECT_EQ(queued_response->result(), http::status::ok);
    EXPECT_FALSE(first_response->keep_alive());
    EXPECT_FALSE(queued_response->keep_alive());
}

TEST(HttpServerGracefulShutdownTest, GraceDeadlineClosesConnectionButStillJoinsRunningHandler) {
    std::mutex handler_mutex;
    std::condition_variable handler_condition;
    bool release_handler = false;
    std::atomic_bool handler_started{false};

    agent::HttpServerOptions options;
    options.port = 0;
    options.shutdown_grace_period = std::chrono::milliseconds(100);

    agent::HttpServer server(options, [&](const agent::HttpRequest&) {
        std::unique_lock lock(handler_mutex);
        handler_started.store(true, std::memory_order_release);
        handler_condition.notify_all();
        handler_condition.wait(lock, [&] { return release_handler; });
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    const unsigned short port = server.Start();
    std::optional<agent::HttpResponse> response;
    std::exception_ptr request_error;
    std::thread client([&] {
        try {
            response = SendGetRequest(port, "/past-grace-period");
        } catch (...) {
            request_error = std::current_exception();
        }
    });

    const bool started = WaitUntil([&] { return handler_started.load(std::memory_order_acquire); }, kConditionTimeout);
    if (!started) {
        {
            std::scoped_lock lock(handler_mutex);
            release_handler = true;
        }
        handler_condition.notify_all();
        client.join();
        server.RequestStop();
        server.Wait();
        FAIL() << "the handler did not start";
        return;
    }

    server.RequestStop();
    std::atomic_bool wait_completed{false};
    std::thread waiter([&] {
        server.Wait();
        wait_completed.store(true, std::memory_order_release);
    });

    const bool connection_closed =
        WaitUntil([&] { return server.ActiveSessionCount() == 0; }, std::chrono::milliseconds(1500));
    EXPECT_TRUE(connection_closed);
    EXPECT_FALSE(wait_completed.load(std::memory_order_acquire));

    {
        std::scoped_lock lock(handler_mutex);
        release_handler = true;
    }
    handler_condition.notify_all();

    client.join();
    waiter.join();

    EXPECT_FALSE(response.has_value());
    EXPECT_NE(request_error, nullptr);
    EXPECT_TRUE(wait_completed.load(std::memory_order_acquire));
    EXPECT_EQ(server.State(), agent::HttpServerState::kStopped);
}

TEST(HttpServerBaselineTest, RecordsConcurrencyBaseline) {
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
