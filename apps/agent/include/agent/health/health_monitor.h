#pragma once

#include "agent/alerts/alert_event.h"
#include "agent/alerts/alert_manager.h"
#include "agent/health/health_checker.h"
#include "agent/health/health_status.h"
#include "agent/recovery/recovery_manager.h"

#include <atomic>
#include <condition_variable>
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

class MetricsCollector;
class ServiceRegistry;

struct HealthMonitorOptions {
    std::chrono::milliseconds interval{2000};
    AlertManagerOptions alert_manager_options{};
    RecoveryManagerOptions recovery_manager_options{};
};

class HealthMonitor {
public:
    HealthMonitor(ServiceRegistry& registry, MetricsCollector& metrics_collector,
                  const HealthMonitorOptions& options = {});

    ~HealthMonitor() noexcept;

    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;

    bool Start(std::string& error);

    void Stop() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

    void CheckOnce();

    [[nodiscard]] std::optional<HealthStatus> GetLatestHealth(std::string_view service_id) const;

    [[nodiscard]] std::vector<HealthStatus> GetAllHealth() const;

    [[nodiscard]] std::vector<AlertEvent> GetActiveAlerts() const;

    [[nodiscard]] std::vector<AlertEvent> GetRecentResolvedAlerts(std::size_t limit) const;

    [[nodiscard]] std::vector<AlertEvent> GetAlertsForService(std::string_view service_id, bool include_resolved) const;

    [[nodiscard]] std::optional<AlertEvent> GetAlert(std::string_view alert_id) const;

    [[nodiscard]] bool AcknowledgeAlert(std::string_view alert_id);

    [[nodiscard]] std::vector<RecoveryEvent> GetRecentRecoveryEvents(std::size_t limit) const;

    [[nodiscard]] std::vector<RecoveryEvent> GetRecoveryEventsForService(std::string_view service_id,
                                                                         std::size_t limit) const;

private:
    void Run(std::stop_token stop_token);

    [[nodiscard]] static UnixTimeMilliseconds NowUnixTimeMilliseconds() noexcept;

private:
    ServiceRegistry& registry_;
    MetricsCollector& metrics_collector_;
    HealthMonitorOptions options_;

    HealthChecker health_checker_;
    AlertEvaluator alert_evaluator_;
    AlertManager alert_manager_;
    RecoveryManager recovery_manager_;

    std::mutex check_mutex_;

    mutable std::shared_mutex health_mutex_;
    std::unordered_map<std::string, HealthStatus> latest_health_;

    mutable std::mutex lifecycle_mutex_;
    std::jthread worker_;

    std::atomic_bool stop_requested_{false};

    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
};
} // namespace aegis::agent