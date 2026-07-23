#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace aegis::test {

struct HttpBenchmarkScenario {
    std::string name;
    std::string description;
    std::size_t concurrent_clients{1};
    std::size_t requests_per_client{1};
    bool keep_alive{false};
    std::chrono::milliseconds handler_delay{0};
    std::size_t io_thread_count{2};
    std::size_t handler_thread_count{4};
};

struct HttpBenchmarkResult {
    HttpBenchmarkScenario scenario;
    std::size_t total_requests{0};
    std::size_t successful_requests{0};
    std::size_t failed_requests{0};
    double elapsed_milliseconds{0.0};
    double requests_per_second{0.0};
    double p50_milliseconds{0.0};
    double p95_milliseconds{0.0};
    double p99_milliseconds{0.0};
    std::size_t maximum_active_handlers{0};
    std::size_t active_sessions_after_stop{0};
    std::size_t in_flight_requests_after_stop{0};
};

[[nodiscard]] HttpBenchmarkResult RunHttpBenchmark(const HttpBenchmarkScenario& scenario);

} // namespace aegis::test
