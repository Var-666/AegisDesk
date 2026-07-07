#pragma once

#include "agent/procfs_reader.h"
#include "agent/service_metrics.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <sys/types.h>

namespace aegis::agent {
class MetricsSampler {
public:
    [[nodiscard]] std::optional<ServiceMetrics> Sample(std::string_view service_id, pid_t pid,
                                                       const ProcessRawStats& process_stats,
                                                       const SystemCpuStats& system_stats,
                                                       UnixTimeMilliseconds collected_at_unix_ms, std::string& error);

    void ForgetService(std::string_view service_id);

    void Clear();

private:
    struct CpuBaseline {
        pid_t pid{-1};

        std::uint64_t start_time_ticks{0};

        std::uint64_t process_cpu_ticks{0};
        std::uint64_t system_cpu_ticks{0};
    };
    std::unordered_map<std::string, CpuBaseline> baselines_;
};
} // namespace aegis::agent