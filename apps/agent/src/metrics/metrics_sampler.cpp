#include "agent/metrics/metrics_sampler.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace aegis::agent {

std::optional<ServiceMetrics> MetricsSampler::Sample(const std::string_view service_id, const pid_t pid,
                                                     const ProcessRawStats& process_stats,
                                                     const SystemCapacitySnapshot& system_capacity,
                                                     const UnixTimeMilliseconds collected_at_unix_ms,
                                                     std::string& error) {
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

    if (system_capacity.monotonic_time_ns == 0) {
        error = "monotonic_time_ns must be greater than zero";
        return std::nullopt;
    }

    if (system_capacity.logical_cpu_count == 0) {
        error = "logical_cpu_count must be greater than zero";
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
        .process_identity = process_stats.process_identity,
        .process_cpu_time_ns = process_stats.process_cpu_time_ns,
        .monotonic_time_ns = system_capacity.monotonic_time_ns,
        .logical_cpu_count = system_capacity.logical_cpu_count,
    };

    const auto iterator = baselines_.find(metrics.service_id);

    // 第一次采样：只建立基线。
    if (iterator == baselines_.end()) {
        baselines_.emplace(metrics.service_id, current_baseline);

        return metrics;
    }

    const CpuBaseline previous_baseline = iterator->second;

    const bool process_identity_changed =
        previous_baseline.pid != pid || previous_baseline.process_identity != process_stats.process_identity;

    const bool cpu_capacity_changed = previous_baseline.logical_cpu_count != system_capacity.logical_cpu_count;

    const bool process_cpu_time_rolled_back = process_stats.process_cpu_time_ns < previous_baseline.process_cpu_time_ns;

    const bool monotonic_time_rolled_back_or_stalled =
        system_capacity.monotonic_time_ns <= previous_baseline.monotonic_time_ns;

    // 以下场景不能安全计算本轮 CPU：
    // 1. 服务重启或 PID 被复用；
    // 2. 在线逻辑 CPU 数变化；
    // 3. 计数器异常回退；
    // 4. 单调时间没有前进。
    if (process_identity_changed || cpu_capacity_changed || process_cpu_time_rolled_back
        || monotonic_time_rolled_back_or_stalled) {
        iterator->second = current_baseline;
        return metrics;
    }

    const std::uint64_t process_cpu_delta_ns =
        process_stats.process_cpu_time_ns - previous_baseline.process_cpu_time_ns;

    const std::uint64_t wall_time_delta_ns = system_capacity.monotonic_time_ns - previous_baseline.monotonic_time_ns;

    // 无论是否成功计算，都先更新基线。
    iterator->second = current_baseline;

    const long double capacity_time_ns =
        static_cast<long double>(wall_time_delta_ns) * static_cast<long double>(system_capacity.logical_cpu_count);

    if (!std::isfinite(capacity_time_ns) || capacity_time_ns <= 0.0L) {
        return metrics;
    }

    const long double cpu_percent = static_cast<long double>(process_cpu_delta_ns) * 100.0L / capacity_time_ns;

    // 单个进程不应超过整机总 CPU 容量。
    // 若平台计数精度或采样时序造成异常，不返回伪造值。
    if (!std::isfinite(cpu_percent) || cpu_percent < 0.0L || cpu_percent > 100.0L) {
        return metrics;
    }

    metrics.cpu_percent = static_cast<double>(cpu_percent);

    return metrics;
}

void MetricsSampler::ForgetService(const std::string_view service_id) {
    baselines_.erase(std::string(service_id));
}

void MetricsSampler::Clear() {
    baselines_.clear();
}

} // namespace aegis::agent