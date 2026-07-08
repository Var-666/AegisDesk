#pragma once

#include "agent/platform_metrics_reader.h"
#include "agent/service_metrics.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <sys/types.h>

namespace aegis::agent {

// 将两次原始采样转换为 CPU 百分比。
//
// 当前设计为单线程使用。
// 后续由 MetricsCollector 的采集线程独占调用。
class MetricsSampler {
public:
    [[nodiscard]] std::optional<ServiceMetrics> Sample(std::string_view service_id, pid_t pid,
                                                       const ProcessRawStats& process_stats,
                                                       const SystemCapacitySnapshot& system_capacity,
                                                       UnixTimeMilliseconds collected_at_unix_ms, std::string& error);

    void ForgetService(std::string_view service_id);

    void Clear();

private:
    struct CpuBaseline {
        pid_t pid{-1};
        std::uint64_t process_identity{0};
        std::uint64_t process_cpu_time_ns{0};
        std::uint64_t monotonic_time_ns{0};
        std::uint32_t logical_cpu_count{0};
    };

    std::unordered_map<std::string, CpuBaseline> baselines_;
};

} // namespace aegis::agent