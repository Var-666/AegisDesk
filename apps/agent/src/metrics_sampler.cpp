#include "agent/metrics_sampler.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace aegis::agent {

std::optional<ServiceMetrics> MetricsSampler::Sample(std::string_view service_id, pid_t pid,
                                                     const ProcessRawStats& process_stats,
                                                     const SystemCpuStats& system_stats,
                                                     UnixTimeMilliseconds collected_at_unix_ms, std::string& error) {
    error.clear();

    if (!IsValidServiceId(service_id)) {
        error = "invalid service_id: " + std::string(service_id);
        return std::nullopt;
    }

    if (pid <= 0) {
        error = "pid must be greater than zero";
        return std::nullopt;
    }

    if (collected_at_unix_ms < 0) {
        error = "collected_at_unix_ms must not be negative";
        return std::nullopt;
    }

    if (system_stats.total_cpu_ticks == 0) {
        error = "system total CPU ticks must be greater than zero";
        return std::nullopt;
    }

    ServiceMetrics metrics{
        .service_id = std::string(service_id),
        .available = true,
        .pid = static_cast<std::int64_t>(pid),
        .collected_at_unix_ms = collected_at_unix_ms,
        .cpu_percent = std::nullopt,
        .rss_bytes = process_stats.rss_bytes,
        .thread_count = process_stats.thread_count,
        .fd_count = process_stats.fd_count,
    };

    const CpuBaseline current_baseline{
        .pid = pid,
        .start_time_ticks = process_stats.start_time_ticks,
        .process_cpu_ticks = process_stats.process_cpu_ticks,
        .system_cpu_ticks = system_stats.total_cpu_ticks,
    };

    const auto iterator = baselines_.find(metrics.service_id);

    // 第一次采样：只建立 CPU 基线。
    if (iterator == baselines_.end()) {
        baselines_.emplace(metrics.service_id, current_baseline);

        return metrics;
    }

    const CpuBaseline previous_baseline = iterator->second;

    const bool process_identity_changed =
        previous_baseline.pid != pid || previous_baseline.start_time_ticks != process_stats.start_time_ticks;

    if (process_identity_changed) {
        iterator->second = current_baseline;
        return metrics;
    }

    const bool process_ticks_rolled_back = process_stats.process_cpu_ticks < previous_baseline.process_cpu_ticks;

    const bool system_ticks_rolled_back_or_stalled = system_stats.total_cpu_ticks <= previous_baseline.system_cpu_ticks;

    if (process_ticks_rolled_back || system_ticks_rolled_back_or_stalled) {
        iterator->second = current_baseline;
        return metrics;
    }

    const std::uint64_t process_tick_delta = process_stats.process_cpu_ticks - previous_baseline.process_cpu_ticks;
    const std::uint64_t system_tick_delta = system_stats.total_cpu_ticks - previous_baseline.system_cpu_ticks;

    iterator->second = current_baseline;

    if (process_tick_delta > system_tick_delta) {
        return metrics;
    }

    const double cpu_percent = static_cast<double>(process_tick_delta) * 100.0 / static_cast<double>(system_tick_delta);

    if (!std::isfinite(cpu_percent) || cpu_percent < 0.0 || cpu_percent > 100.0) {
        return metrics;
    }

    metrics.cpu_percent = cpu_percent;

    return metrics;
}

void MetricsSampler::ForgetService(const std::string_view service_id) {
    baselines_.erase(std::string(service_id));
}

void MetricsSampler::Clear() {
    baselines_.clear();
}
} // namespace aegis::agent