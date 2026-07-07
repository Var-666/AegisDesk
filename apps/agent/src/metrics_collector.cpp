#include "agent/metrics_collector.h"

#include "agent/procfs_reader.h"
#include "agent/service_registry.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegis::agent {

MetricsCollector::MetricsCollector(ServiceRegistry& registry, MetricsCollectorOptions options)
    : registry_(registry)
    , options_(options) {}

MetricsCollector::~MetricsCollector() noexcept {
    Stop();
}

bool MetricsCollector::Start(std::string& error) {
    error.clear();

    if (options_.interval <= std::chrono::milliseconds::zero()) {
        error = "metrics collection interval must be greater than zero";
        return false;
    }

    if (options_.history_capacity == 0) {
        error = "metrics history capacity must be greater than zero";
        return false;
    }

    std::scoped_lock lock(lifecycle_mutex_);

    if (worker_.joinable()) {
        error = "metrics collector is already running";
        return false;
    }

    stop_requested_.store(false, std::memory_order_release);

    try {
        worker_ = std::jthread([this](const std::stop_token stop_token) { Run(stop_token); });
    } catch (const std::system_error& exception) {
        error = std::string("failed to start metrics collector: ") + exception.what();
        return false;
    }

    return true;
}

void MetricsCollector::Stop() noexcept {
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

bool MetricsCollector::IsRunning() const noexcept {
    std::scoped_lock lock(lifecycle_mutex_);

    return worker_.joinable() && !stop_requested_.load(std::memory_order_acquire);
}

std::optional<ServiceMetrics> MetricsCollector::GetLatest(const std::string_view service_id) const {
    std::shared_lock lock(metrics_mutex_);

    const auto iterator = latest_metrics_.find(std::string(service_id));

    if (iterator == latest_metrics_.end()) {
        return std::nullopt;
    }

    return iterator->second;
}

std::optional<std::vector<ServiceMetrics>> MetricsCollector::GetHistory(const std::string_view service_id,
                                                                        const std::size_t limit) const {
    std::shared_lock lock(metrics_mutex_);

    const auto iterator = metrics_history_.find(std::string(service_id));

    if (iterator == metrics_history_.end()) {
        return std::nullopt;
    }

    const std::deque<ServiceMetrics>& history = iterator->second;

    if (limit == 0 || history.empty()) {
        return std::vector<ServiceMetrics>{};
    }

    const std::size_t start_index = history.size() > limit ? history.size() - limit : 0;

    std::vector<ServiceMetrics> result;

    result.reserve(history.size() - start_index);

    for (std::size_t index = start_index; index < history.size(); ++index) {
        result.push_back(history[index]);
    }

    return result;
}

void MetricsCollector::CollectOnce() {
    std::scoped_lock collection_lock(collection_mutex_);

    const UnixTimeMilliseconds collected_at_unix_ms = NowUnixTimeMilliseconds();

    const std::vector<ServiceSummary> services = registry_.ListServices();

    bool has_running_service = false;

    for (const ServiceSummary& service : services) {
        if (service.status.state == ServiceState::kRunning && service.status.pid > 0) {
            has_running_service = true;
            break;
        }
    }

    std::optional<SystemCpuStats> system_cpu_stats;

    if (has_running_service) {
        std::string system_cpu_error;

        system_cpu_stats = ReadSystemCpuStats(system_cpu_error);
    }

    std::unordered_map<std::string, ServiceMetrics> next_metrics;

    next_metrics.reserve(services.size());

    for (const ServiceSummary& service : services) {
        const std::int64_t pid = static_cast<std::int64_t>(service.status.pid);

        const bool service_running = service.status.state == ServiceState::kRunning && service.status.pid > 0;

        if (!service_running) {
            sampler_.ForgetService(service.id);
            next_metrics.emplace(service.id, MakeUnavailableMetrics(service.id, -1, collected_at_unix_ms));
            continue;
        }

        if (!system_cpu_stats.has_value()) {
            sampler_.ForgetService(service.id);
            next_metrics.emplace(service.id, MakeUnavailableMetrics(service.id, pid, collected_at_unix_ms));
            continue;
        }

        std::string process_error;

        const std::optional<ProcessRawStats> process_stats = ReadProcessRawStats(service.status.pid, process_error);

        if (!process_stats.has_value()) {
            sampler_.ForgetService(service.id);
            next_metrics.emplace(service.id, MakeUnavailableMetrics(service.id, pid, collected_at_unix_ms));
            continue;
        }

        std::string sampler_error;

        const std::optional<ServiceMetrics> metrics = sampler_.Sample(
            service.id, service.status.pid, *process_stats, *system_cpu_stats, collected_at_unix_ms, sampler_error);

        if (!metrics.has_value()) {
            sampler_.ForgetService(service.id);
            next_metrics.emplace(service.id, MakeUnavailableMetrics(service.id, pid, collected_at_unix_ms));
            continue;
        }

        next_metrics.emplace(service.id, *metrics);
    }

    {
        std::unique_lock lock(metrics_mutex_);

        for (const auto& [service_id, metrics] : next_metrics) {
            std::deque<ServiceMetrics>& history = metrics_history_[service_id];

            history.push_back(metrics);

            while (history.size() > options_.history_capacity) {
                history.pop_front();
            }
        }

        for (auto iterator = metrics_history_.begin(); iterator != metrics_history_.end();) {
            if (!next_metrics.contains(iterator->first)) {
                iterator = metrics_history_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        latest_metrics_ = std::move(next_metrics);
    }
}

void MetricsCollector::Run(const std::stop_token stop_token) {
    while (!stop_token.stop_requested() && !stop_requested_.load(std::memory_order_acquire)) {
        CollectOnce();

        std::unique_lock lock(wait_mutex_);

        wait_cv_.wait_for(lock, options_.interval, [this, &stop_token] {
            return stop_token.stop_requested() || stop_requested_.load(std::memory_order_acquire);
        });
    }
}

UnixTimeMilliseconds MetricsCollector::NowUnixTimeMilliseconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return milliseconds < 0 ? 0 : static_cast<UnixTimeMilliseconds>(milliseconds);
}

ServiceMetrics MetricsCollector::MakeUnavailableMetrics(std::string service_id, const std::int64_t pid,
                                                        const UnixTimeMilliseconds collected_at_unix_ms) {
    return ServiceMetrics{
        .service_id = std::move(service_id),
        .available = false,
        .pid = pid,
        .collected_at_unix_ms = collected_at_unix_ms,
        .cpu_percent = std::nullopt,
        .rss_bytes = std::nullopt,
        .thread_count = std::nullopt,
        .fd_count = std::nullopt,
    };
}

} // namespace aegis::agent