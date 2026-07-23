#include "performance/http_benchmark_runner.h"

#include "agent/api/http_json.h"
#include "agent/api/http_server.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <limits>
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

constexpr auto kClientTimeout = std::chrono::seconds(10);

void UpdateMaximum(std::atomic_size_t& maximum, const std::size_t candidate) {
    std::size_t observed = maximum.load(std::memory_order_relaxed);
    while (
        candidate > observed
        && !maximum.compare_exchange_weak(observed, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
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

[[nodiscard]] bool IsSuccessful(const agent::HttpResponse& response) {
    return response.result() == http::status::ok && response.body() == R"({"status":"ok"})";
}

[[nodiscard]] double Percentile(const std::vector<double>& sorted_values, const double quantile) {
    if (sorted_values.empty()) {
        return 0.0;
    }

    const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(sorted_values.size())));
    return sorted_values[std::max<std::size_t>(rank, 1U) - 1U];
}

void ValidateScenario(const HttpBenchmarkScenario& scenario) {
    if (scenario.name.empty()) {
        throw std::invalid_argument("benchmark scenario name must not be empty");
    }
    if (scenario.concurrent_clients == 0 || scenario.requests_per_client == 0) {
        throw std::invalid_argument("benchmark client and request counts must be greater than zero");
    }
    if (scenario.io_thread_count == 0 || scenario.handler_thread_count == 0) {
        throw std::invalid_argument("benchmark worker counts must be greater than zero");
    }
    if (scenario.requests_per_client > std::numeric_limits<std::size_t>::max() / scenario.concurrent_clients) {
        throw std::overflow_error("benchmark request count overflow");
    }
}

} // namespace

HttpBenchmarkResult RunHttpBenchmark(const HttpBenchmarkScenario& scenario) {
    ValidateScenario(scenario);

    const std::size_t total_requests = scenario.concurrent_clients * scenario.requests_per_client;
    std::atomic_size_t active_handlers{0};
    std::atomic_size_t maximum_active_handlers{0};

    agent::HttpServerOptions server_options;
    server_options.port = 0;
    server_options.io_thread_count = scenario.io_thread_count;
    server_options.handler_thread_count = scenario.handler_thread_count;
    server_options.max_connections = std::max<std::size_t>(128U, scenario.concurrent_clients * 2U);
    server_options.max_in_flight_requests = std::max<std::size_t>(64U, scenario.concurrent_clients * 2U);
    server_options.max_requests_per_connection = std::max<std::size_t>(100U, scenario.requests_per_client);

    agent::HttpServer server(server_options, [&](const agent::HttpRequest&) {
        const std::size_t active = active_handlers.fetch_add(1, std::memory_order_acq_rel) + 1U;
        UpdateMaximum(maximum_active_handlers, active);
        if (scenario.handler_delay > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(scenario.handler_delay);
        }
        active_handlers.fetch_sub(1, std::memory_order_acq_rel);
        return agent::MakeJsonResponse(http::status::ok, R"({"status":"ok"})");
    });
    const unsigned short port = server.Start();
    const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);

    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    std::size_t ready_clients = 0;
    bool start = false;

    std::atomic_size_t successful_requests{0};
    std::atomic_size_t failed_requests{0};
    std::vector<std::optional<double>> request_latencies(total_requests);
    std::vector<std::thread> clients;
    clients.reserve(scenario.concurrent_clients);

    for (std::size_t client_index = 0; client_index < scenario.concurrent_clients; ++client_index) {
        clients.emplace_back([&, client_index] {
            asio::io_context io_context;
            beast::tcp_stream persistent_stream(io_context);
            bool persistent_connection_available = false;

            // Keep-Alive scenarios measure request reuse after connection setup. Short-connection scenarios
            // intentionally keep connect latency inside each request measurement.
            if (scenario.keep_alive) {
                try {
                    persistent_stream.expires_after(kClientTimeout);
                    persistent_stream.connect(endpoint);
                    persistent_connection_available = true;
                } catch (const std::exception&) {
                    persistent_connection_available = false;
                }
            }

            {
                std::unique_lock lock(gate_mutex);
                ++ready_clients;
                gate_condition.notify_all();
                gate_condition.wait(lock, [&] { return start; });
            }

            for (std::size_t request_index = 0; request_index < scenario.requests_per_client; ++request_index) {
                const std::size_t result_index = client_index * scenario.requests_per_client + request_index;
                const auto request_started_at = std::chrono::steady_clock::now();

                try {
                    agent::HttpRequest request{
                        http::verb::get,
                        "/benchmark/" + std::to_string(client_index) + "/" + std::to_string(request_index), 11};
                    request.set(http::field::host, "127.0.0.1");
                    request.set(http::field::user_agent, "aegis-http-benchmark");
                    request.keep_alive(scenario.keep_alive);

                    agent::HttpResponse response;
                    if (scenario.keep_alive) {
                        if (!persistent_connection_available) {
                            throw std::runtime_error("persistent connection is unavailable");
                        }
                        response = ExchangeRequest(persistent_stream, std::move(request));
                    } else {
                        asio::io_context request_context;
                        beast::tcp_stream request_stream(request_context);
                        request_stream.expires_after(kClientTimeout);
                        request_stream.connect(endpoint);
                        response = ExchangeRequest(request_stream, std::move(request));
                    }

                    const auto request_finished_at = std::chrono::steady_clock::now();
                    if (IsSuccessful(response)) {
                        request_latencies[result_index] =
                            std::chrono::duration<double, std::milli>(request_finished_at - request_started_at).count();
                        successful_requests.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        failed_requests.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (const std::exception&) {
                    persistent_connection_available = false;
                    failed_requests.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    {
        std::unique_lock lock(gate_mutex);
        gate_condition.wait(lock, [&] { return ready_clients == scenario.concurrent_clients; });
        start = true;
    }

    const auto batch_started_at = std::chrono::steady_clock::now();
    gate_condition.notify_all();
    for (std::thread& client : clients) {
        client.join();
    }
    const auto batch_finished_at = std::chrono::steady_clock::now();

    server.RequestStop();
    server.Wait();

    std::vector<double> successful_latencies;
    successful_latencies.reserve(successful_requests.load(std::memory_order_relaxed));
    for (const std::optional<double> latency : request_latencies) {
        if (latency.has_value()) {
            successful_latencies.push_back(*latency);
        }
    }
    std::ranges::sort(successful_latencies);

    const double elapsed_milliseconds =
        std::chrono::duration<double, std::milli>(batch_finished_at - batch_started_at).count();
    const double elapsed_seconds = elapsed_milliseconds / 1000.0;

    return {
        .scenario = scenario,
        .total_requests = total_requests,
        .successful_requests = successful_requests.load(std::memory_order_relaxed),
        .failed_requests = failed_requests.load(std::memory_order_relaxed),
        .elapsed_milliseconds = elapsed_milliseconds,
        .requests_per_second =
            elapsed_seconds > 0.0
                ? static_cast<double>(successful_requests.load(std::memory_order_relaxed)) / elapsed_seconds
                : 0.0,
        .p50_milliseconds = Percentile(successful_latencies, 0.50),
        .p95_milliseconds = Percentile(successful_latencies, 0.95),
        .p99_milliseconds = Percentile(successful_latencies, 0.99),
        .maximum_active_handlers = maximum_active_handlers.load(std::memory_order_relaxed),
        .active_sessions_after_stop = server.ActiveSessionCount(),
        .in_flight_requests_after_stop = server.InFlightRequestCount(),
    };
}

} // namespace aegis::test
