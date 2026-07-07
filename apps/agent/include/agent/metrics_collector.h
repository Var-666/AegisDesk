#pragma once

#include "agent/metrics_sampler.h"
#include "agent/service_metrics.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aegis::agent {

class ServiceRegistry;

struct MetricsCollectorOptions {
    std::chrono::milliseconds interval{1000};
    std::size_t history_capacity{300};
};

class MetricsCollector {
public:
    explicit MetricsCollector(ServiceRegistry& registry, MetricsCollectorOptions options = {});

    ~MetricsCollector() noexcept;

    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;

    bool Start(std::string& error);

    void Stop() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

    [[nodiscard]] std::optional<ServiceMetrics> GetLatest(std::string_view service_id) const;

    [[nodiscard]] std::optional<std::vector<ServiceMetrics>> GetHistory(std::string_view service_id,
                                                                        std::size_t limit) const;

    void CollectOnce();

private:
    void Run(std::stop_token stop_token);

    [[nodiscard]] static UnixTimeMilliseconds NowUnixTimeMilliseconds() noexcept;

    [[nodiscard]] static ServiceMetrics MakeUnavailableMetrics(std::string service_id, std::int64_t pid,
                                                               UnixTimeMilliseconds collected_at_unix_ms);

    ServiceRegistry& registry_;
    MetricsCollectorOptions options_;

    MetricsSampler sampler_;

    std::mutex collection_mutex_;

    mutable std::shared_mutex metrics_mutex_;

    std::unordered_map<std::string, ServiceMetrics> latest_metrics_;

    std::unordered_map<std::string, std::deque<ServiceMetrics>> metrics_history_;

    mutable std::mutex lifecycle_mutex_;
    std::jthread worker_;

    std::atomic_bool stop_requested_{false};

    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
};

} // namespace aegis::agent