#include "performance/http_benchmark_runner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>

namespace aegis::test {
namespace {

void ExpectCleanResult(const HttpBenchmarkResult& result) {
    EXPECT_EQ(result.successful_requests, result.total_requests);
    EXPECT_EQ(result.failed_requests, 0U);
    EXPECT_EQ(result.active_sessions_after_stop, 0U);
    EXPECT_EQ(result.in_flight_requests_after_stop, 0U);
}

TEST(HttpServerPerformanceAcceptanceTest, HandlerPoolProvidesObservableParallelSpeedup) {
    constexpr std::size_t kClients = 16;
    constexpr auto kHandlerDelay = std::chrono::milliseconds(40);

    const HttpBenchmarkResult result = RunHttpBenchmark({
        .name = "handler-pool-acceptance",
        .description = "Verify that four Handler workers execute simulated business work concurrently.",
        .concurrent_clients = kClients,
        .requests_per_client = 1,
        .keep_alive = false,
        .handler_delay = kHandlerDelay,
        .io_thread_count = 1,
        .handler_thread_count = 4,
    });

    const auto serial_duration = kHandlerDelay * kClients;
    ExpectCleanResult(result);
    EXPECT_GE(result.maximum_active_handlers, 3U);
    EXPECT_LE(result.maximum_active_handlers, 4U);
    EXPECT_LT(result.elapsed_milliseconds, static_cast<double>(serial_duration.count()) * 0.75);
}

TEST(HttpServerPerformanceAcceptanceTest, SustainsKeepAliveLoadWithStableResources) {
    const HttpBenchmarkResult result = RunHttpBenchmark({
        .name = "keep-alive-acceptance",
        .description = "Exercise sustained request reuse across multiple concurrent connections.",
        .concurrent_clients = 16,
        .requests_per_client = 50,
        .keep_alive = true,
        .handler_delay = std::chrono::milliseconds::zero(),
        .io_thread_count = 2,
        .handler_thread_count = 4,
    });

    ExpectCleanResult(result);
    EXPECT_GE(result.requests_per_second, 100.0);
    EXPECT_LT(result.p95_milliseconds, 2000.0);
}

TEST(HttpServerPerformanceAcceptanceTest, ServesFiftyConcurrentShortConnections) {
    const HttpBenchmarkResult result = RunHttpBenchmark({
        .name = "short-connection-acceptance",
        .description = "Verify the target 50-client short-connection scenario.",
        .concurrent_clients = 50,
        .requests_per_client = 1,
        .keep_alive = false,
        .handler_delay = std::chrono::milliseconds::zero(),
        .io_thread_count = 2,
        .handler_thread_count = 4,
    });

    ExpectCleanResult(result);
    EXPECT_LT(result.elapsed_milliseconds, 3000.0);
}

} // namespace
} // namespace aegis::test
