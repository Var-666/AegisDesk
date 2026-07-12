#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <sys/types.h>

namespace aegis::agent {
struct ProcessRawStats {
    std::uint64_t process_cpu_time_ns{0};
    std::uint64_t process_identity{0};
    std::uint64_t rss_bytes{0};
    std::uint64_t thread_count{0};
    std::uint64_t fd_count{0};
};

struct SystemCapacitySnapshot {
    std::uint64_t monotonic_time_ns{0};
    std::uint32_t logical_cpu_count{0};
};

class PlatformMetricsReader {
public:
    virtual ~PlatformMetricsReader() = default;

    virtual std::optional<ProcessRawStats> ReadProcessRawStats(pid_t pid, std::string& error) = 0;

    virtual std::optional<SystemCapacitySnapshot> ReadSystemCapacity(std::string& error) = 0;
};

[[nodiscard]] std::unique_ptr<PlatformMetricsReader> CreatePlatformMetricsReader();

} // namespace aegis::agent