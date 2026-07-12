#include "agent/health/health_monitor.h"

#include "agent/alerts/alert_evaluator.h"
#include "agent/metrics/metrics_collector.h"
#include "agent/service/process_supervisor.h"
#include "agent/service/service_registry.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace aegis::agent {

HealthMonitor::HealthMonitor(ServiceRegistry& registry, MetricsCollector& metrics_collector,
                             const HealthMonitorOptions& options)
    : registry_(registry)
    , metrics_collector_(metrics_collector)
    , options_(options)
    , alert_manager_(options_.alert_manager_options)
    , recovery_manager_(options_.recovery_manager_options) {}

HealthMonitor::~HealthMonitor() noexcept {
    Stop();
}

bool HealthMonitor::Start(std::string& error) {
    error.clear();

    if (options_.interval <= std::chrono::milliseconds::zero()) {
        error = "health monitor interval must be greater than zero";
        return false;
    }

    std::scoped_lock lock(lifecycle_mutex_);

    if (worker_.joinable()) {
        error = "health monitor is already running";
        return false;
    }

    stop_requested_.store(false, std::memory_order_release);

    try {
        worker_ = std::jthread([this](const std::stop_token& stop_token) { Run(stop_token); });
    } catch (const std::system_error& exception) {
        error = std::string("failed to start health monitor: ") + exception.what();
        return false;
    }

    return true;
}

void HealthMonitor::Stop() noexcept {
    std::jthread worker_to_join;

    {
        std::scoped_lock lock(lifecycle_mutex_);

        stop_requested_.store(true, std::memory_order_release);

        if (!worker_.joinable()) {
            return;
        }

        worker_.request_stop();
        wait_cv_.notify_all();

        if (worker_.get_id() == std::this_thread::get_id()) {
            return;
        }

        worker_to_join = std::move(worker_);
    }

    try {
        worker_to_join.join();
    } catch (...) {}
}

bool HealthMonitor::IsRunning() const noexcept {
    std::scoped_lock lock(lifecycle_mutex_);

    return worker_.joinable() && !stop_requested_.load(std::memory_order_acquire);
}

void HealthMonitor::CheckOnce() {
    std::scoped_lock check_lock(check_mutex_);

    const UnixTimeMilliseconds checked_at_unix_ms = NowUnixTimeMilliseconds();

    const std::vector<ServiceSummary> services = registry_.ListServices();

    std::unordered_map<std::string, HealthStatus> next_health;
    next_health.reserve(services.size());

    std::unordered_set<std::string> seen_service_ids;
    seen_service_ids.reserve(services.size());

    std::vector<AlertEvaluation> all_evaluations;

    for (const ServiceSummary& service : services) {
        seen_service_ids.insert(service.id);

        ProcessSupervisor* supervisor = registry_.Find(service.id);

        if (supervisor == nullptr) {
            HealthStatus unknown{
                .service_id = service.id,
                .state = HealthState::kUnknown,
                .reason = "service supervisor was not found",
                .consecutive_failures = 0,
                .checked_at_unix_ms = checked_at_unix_ms,
            };

            next_health.emplace(service.id, unknown);
            continue;
        }

        const ServiceDefinition& definition = supervisor->Definition();

        const HealthStatus health_status = health_checker_.Check(definition, service.status, checked_at_unix_ms);

        next_health.emplace(definition.id, health_status);

        const std::optional<ServiceMetrics> metrics = metrics_collector_.GetLatest(definition.id);

        std::vector<AlertEvaluation> evaluations =
            alert_evaluator_.Evaluate(definition, metrics, health_status, checked_at_unix_ms);

        all_evaluations.insert(all_evaluations.end(), evaluations.begin(), evaluations.end());
    }

    alert_manager_.ApplyEvaluations(all_evaluations);

    for (const ServiceSummary& service : services) {
        ProcessSupervisor* supervisor = registry_.Find(service.id);

        if (supervisor == nullptr) {
            continue;
        }

        const auto health_iterator = next_health.find(service.id);

        if (health_iterator == next_health.end()) {
            continue;
        }

        const std::vector<AlertEvent> active_alerts = alert_manager_.GetAlertsForService(service.id, false);

        recovery_manager_.EvaluateAndRecover(*supervisor, health_iterator->second, active_alerts, checked_at_unix_ms);
    }

    {
        std::unique_lock lock(health_mutex_);

        for (const auto& [service_id, _] : latest_health_) {
            if (!seen_service_ids.contains(service_id)) {
                health_checker_.ForgetService(service_id);
                alert_evaluator_.ForgetService(service_id);
                alert_manager_.ForgetService(service_id);
                recovery_manager_.ForgetService(service_id);
            }
        }

        latest_health_ = std::move(next_health);
    }
}

std::optional<HealthStatus> HealthMonitor::GetLatestHealth(const std::string_view service_id) const {
    std::shared_lock lock(health_mutex_);

    const auto iterator = latest_health_.find(std::string(service_id));

    if (iterator == latest_health_.end()) {
        return std::nullopt;
    }

    return iterator->second;
}

std::vector<HealthStatus> HealthMonitor::GetAllHealth() const {
    std::shared_lock lock(health_mutex_);

    std::vector<HealthStatus> result;

    result.reserve(latest_health_.size());

    for (const auto& [_, status] : latest_health_) {
        result.push_back(status);
    }

    std::ranges::sort(
        result, [](const HealthStatus& left, const HealthStatus& right) { return left.service_id < right.service_id; });

    return result;
}

std::vector<AlertEvent> HealthMonitor::GetActiveAlerts() const {
    return alert_manager_.GetActiveAlerts();
}

std::vector<AlertEvent> HealthMonitor::GetRecentResolvedAlerts(const std::size_t limit) const {
    return alert_manager_.GetRecentResolvedAlerts(limit);
}

std::vector<AlertEvent> HealthMonitor::GetAlertsForService(const std::string_view service_id,
                                                           const bool include_resolved) const {
    return alert_manager_.GetAlertsForService(service_id, include_resolved);
}

std::optional<AlertEvent> HealthMonitor::GetAlert(const std::string_view alert_id) const {
    return alert_manager_.GetAlert(alert_id);
}

bool HealthMonitor::AcknowledgeAlert(const std::string_view alert_id) {
    return alert_manager_.Acknowledge(alert_id);
}

std::vector<RecoveryEvent> HealthMonitor::GetRecentRecoveryEvents(const std::size_t limit) const {
    return recovery_manager_.GetRecentEvents(limit);
}

std::vector<RecoveryEvent> HealthMonitor::GetRecoveryEventsForService(const std::string_view service_id,
                                                                      const std::size_t limit) const {
    return recovery_manager_.GetEventsForService(service_id, limit);
}

void HealthMonitor::Run(const std::stop_token stop_token) {
    while (!stop_token.stop_requested() && !stop_requested_.load(std::memory_order_acquire)) {
        CheckOnce();

        std::unique_lock lock(wait_mutex_);

        wait_cv_.wait_for(lock, options_.interval, [this, &stop_token] {
            return stop_token.stop_requested() || stop_requested_.load(std::memory_order_acquire);
        });
    }
}

UnixTimeMilliseconds HealthMonitor::NowUnixTimeMilliseconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();

    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    if (milliseconds < 0) {
        return 0;
    }

    return static_cast<UnixTimeMilliseconds>(milliseconds);
}

} // namespace aegis::agent