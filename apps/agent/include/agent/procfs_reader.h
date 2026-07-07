#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include <sys/types.h>

namespace aegis::agent {

struct ProcessRawStats {
    std::uint64_t process_cpu_ticks{0};
    std::uint64_t start_time_ticks{0};
    std::uint64_t rss_bytes{0};
    std::uint64_t thread_count{0};
    std::uint64_t fd_count{0};
};

struct SystemCpuStats {
    std::uint64_t total_cpu_ticks{0};
};

[[nodiscard]] std::optional<ProcessRawStats> ReadProcessRawStats(pid_t pid, std::string& error);

[[nodiscard]] std::optional<SystemCpuStats> ReadSystemCpuStats(std::string& error);

} // namespace aegis::agent
