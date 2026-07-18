#include "support/test_builders.h"

#include "agent/alerts/alert_manager.h"
#include "agent/api/agent_api.h"
#include "agent/api/http_json.h"
#include "agent/health/health_monitor.h"
#include "agent/metrics/metrics_collector.h"
#include "agent/service/service_registry.h"

#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace aegis::test {
namespace {

namespace http = boost::beast::http;

struct RequestSpec {
    http::verb method;
    std::string_view target;
};

void WriteServiceConfig(const std::filesystem::path& config_path, const std::filesystem::path& root,
                        const std::filesystem::path& executable, const std::vector<std::string>& args) {
    const std::filesystem::path log_path = root / "concurrent_service.log";

    std::ofstream log(log_path);
    ASSERT_TRUE(log.is_open());
    log << "thread-safe log snapshot\n";
    log.close();

    std::ofstream output(config_path);
    ASSERT_TRUE(output.is_open());
    output << R"({"schema_version":1,"services":[{"id":"concurrent_service",)"
           << R"("display_name":"Concurrent Service","executable":")" << agent::JsonEscape(executable.string())
           << R"(","work_dir":")" << agent::JsonEscape(root.string()) << R"(","args":[)";

    for (std::size_t index = 0; index < args.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << '"' << agent::JsonEscape(args[index]) << '"';
    }

    output << R"(],"log_path":")" << agent::JsonEscape(log_path.string())
           << R"(","auto_start":false,"health_check":{"enabled":true,"type":"process",)"
           << R"("interval_seconds":5,"timeout_milliseconds":1000,"failure_threshold":3}}]})";
}

agent::HttpRequest MakeRequest(const http::verb method, const std::string_view target) {
    agent::HttpRequest request;
    request.method(method);
    request.target(target);
    request.version(11);
    return request;
}

bool WaitUntil(const std::function<bool()>& predicate,
               const std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return predicate();
}

TEST(AgentApiConcurrencyTest, ConcurrentReadsRemainConsistentWhileCollectorsPublishSnapshots) {
    ScopedTempDirectory directory("agent-api-concurrent-reads");
    const std::filesystem::path config_path = directory.Path() / "services.json";
    WriteServiceConfig(config_path, directory.Path(), ServiceDefinitionBuilder::FaultProcessPath(), {});

    agent::ServiceRegistry registry;
    std::string error;
    ASSERT_TRUE(registry.LoadFromFile(config_path, directory.Path(), error)) << error;

    agent::MetricsCollector metrics_collector(registry);
    agent::HealthMonitor health_monitor(registry, metrics_collector);
    metrics_collector.CollectOnce();
    health_monitor.CheckOnce();

    const agent::AgentApi api(registry, metrics_collector, health_monitor);

    constexpr std::array<RequestSpec, 10> requests{{
        {http::verb::get, "/api/v1/services"},
        {http::verb::get, "/api/v1/services/concurrent_service/status"},
        {http::verb::get, "/api/v1/services/concurrent_service/logs?tail=10"},
        {http::verb::get, "/api/v1/services/concurrent_service/metrics"},
        {http::verb::get, "/api/v1/services/concurrent_service/metrics/history?limit=10"},
        {http::verb::get, "/api/v1/services/concurrent_service/health"},
        {http::verb::get, "/api/v1/services/concurrent_service/alerts"},
        {http::verb::get, "/api/v1/services/concurrent_service/recovery-events?limit=10"},
        {http::verb::get, "/api/v1/alerts?include_resolved=true"},
        {http::verb::get, "/api/v1/recovery-events?limit=10"},
    }};

    constexpr std::size_t kReaderCount = 8;
    constexpr std::size_t kIterations = 100;
    std::barrier start_gate(static_cast<std::ptrdiff_t>(kReaderCount + 2));
    std::atomic_size_t failed_responses{0};
    std::vector<std::thread> workers;
    workers.reserve(kReaderCount + 2);

    for (std::size_t reader_index = 0; reader_index < kReaderCount; ++reader_index) {
        workers.emplace_back([&, reader_index] {
            start_gate.arrive_and_wait();

            for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
                const RequestSpec& spec = requests[(reader_index + iteration) % requests.size()];
                const agent::HttpResponse response = api.Handle(MakeRequest(spec.method, spec.target));

                if (response.result() != http::status::ok) {
                    failed_responses.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    workers.emplace_back([&] {
        start_gate.arrive_and_wait();
        for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
            metrics_collector.CollectOnce();
        }
    });

    workers.emplace_back([&] {
        start_gate.arrive_and_wait();
        for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
            health_monitor.CheckOnce();
        }
    });

    for (std::thread& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(failed_responses.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(registry.Size(), 1U);
}

TEST(AgentApiConcurrencyTest, ConcurrentStartRequestsCreateOnlyOneProcess) {
    ScopedTempDirectory directory("agent-api-concurrent-start");
    const std::filesystem::path config_path = directory.Path() / "services.json";
    const std::filesystem::path ready_file = directory.Path() / "ready";
    WriteServiceConfig(config_path, directory.Path(), ServiceDefinitionBuilder::FaultProcessPath(),
                       {"--ready-file", ready_file.string()});

    agent::ServiceRegistry registry;
    std::string error;
    ASSERT_TRUE(registry.LoadFromFile(config_path, directory.Path(), error)) << error;

    agent::MetricsCollector metrics_collector(registry);
    agent::HealthMonitor health_monitor(registry, metrics_collector);
    const agent::AgentApi api(registry, metrics_collector, health_monitor);

    constexpr std::size_t kCallerCount = 8;
    std::barrier start_gate(static_cast<std::ptrdiff_t>(kCallerCount));
    std::atomic_size_t successful_requests{0};
    std::atomic_size_t conflict_responses{0};
    std::atomic_size_t unexpected_responses{0};
    std::vector<std::thread> callers;
    callers.reserve(kCallerCount);

    for (std::size_t index = 0; index < kCallerCount; ++index) {
        callers.emplace_back([&] {
            start_gate.arrive_and_wait();
            const agent::HttpResponse response =
                api.Handle(MakeRequest(http::verb::post, "/api/v1/services/concurrent_service/start"));

            if (response.result() == http::status::ok) {
                successful_requests.fetch_add(1, std::memory_order_relaxed);
            } else if (response.result() == http::status::conflict) {
                conflict_responses.fetch_add(1, std::memory_order_relaxed);
            } else {
                unexpected_responses.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::thread& caller : callers) {
        caller.join();
    }

    EXPECT_EQ(successful_requests.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(conflict_responses.load(std::memory_order_relaxed), kCallerCount - 1);
    EXPECT_EQ(unexpected_responses.load(std::memory_order_relaxed), 0U);
    EXPECT_TRUE(WaitUntil([&] { return std::filesystem::exists(ready_file); }));

    const agent::HttpResponse stop_response =
        api.Handle(MakeRequest(http::verb::post, "/api/v1/services/concurrent_service/stop"));
    EXPECT_EQ(stop_response.result(), http::status::ok);
}

TEST(AlertManagerConcurrencyContractTest, AcknowledgeReturnsTheSnapshotMutatedUnderTheSameLock) {
    agent::AlertManager alert_manager;
    const agent::AlertEvaluation evaluation{
        .service_id = "concurrent_service",
        .rule_id = "high_cpu",
        .severity = agent::AlertSeverity::kWarning,
        .rule_enabled = true,
        .condition_met = true,
        .firing = true,
        .evaluated_at_unix_ms = 1000,
        .condition_started_at_unix_ms = 1000,
        .message = "CPU usage is high",
    };

    const agent::AlertManagerUpdate update = alert_manager.ApplyEvaluations({evaluation});
    ASSERT_EQ(update.created.size(), 1U);

    const std::optional<agent::AlertEvent> acknowledged = alert_manager.AcknowledgeAndGet("concurrent_service:high_cpu");
    ASSERT_TRUE(acknowledged.has_value());
    EXPECT_TRUE(acknowledged->acknowledged);
    EXPECT_EQ(acknowledged->id, "concurrent_service:high_cpu");
}

} // namespace
} // namespace aegis::test
