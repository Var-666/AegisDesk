#include "agent/metrics/platform_metrics_reader.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include <libproc.h>
#include <mach/kern_return.h>
#include <mach/mach_time.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>

namespace aegis::agent {
namespace {

constexpr std::uint64_t kMicrosecondsPerSecond = 1000ULL * 1000ULL;

[[nodiscard]] std::string ErrnoMessage(const std::string& operation) {
    return operation + ": " + std::strerror(errno);
}

[[nodiscard]] bool AddWithoutOverflow(const std::uint64_t left, const std::uint64_t right,
                                      std::uint64_t& result) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }

    result = left + right;
    return true;
}

// macOS 的 pti_total_user / pti_total_system 是 Mach 时间单位。
// 使用 mach_timebase_info() 转换为纳秒。
[[nodiscard]] bool ConvertMachTicksToNanoseconds(const std::uint64_t mach_ticks,
                                                 const mach_timebase_info_data_t& timebase,
                                                 std::uint64_t& nanoseconds) {
    if (timebase.numer == 0 || timebase.denom == 0) {
        return false;
    }

    const std::uint64_t numerator = static_cast<std::uint64_t>(timebase.numer);

    const std::uint64_t denominator = static_cast<std::uint64_t>(timebase.denom);

    const std::uint64_t whole_units = mach_ticks / denominator;

    const std::uint64_t remaining_units = mach_ticks % denominator;

    if (whole_units > std::numeric_limits<std::uint64_t>::max() / numerator) {
        return false;
    }

    const std::uint64_t whole_nanoseconds = whole_units * numerator;

    // numerator / denominator 均来自 uint32_t，
    // 因此 remaining_units * numerator 不会超过 uint64_t。
    const std::uint64_t fractional_nanoseconds = (remaining_units * numerator) / denominator;

    if (fractional_nanoseconds > std::numeric_limits<std::uint64_t>::max() - whole_nanoseconds) {
        return false;
    }

    nanoseconds = whole_nanoseconds + fractional_nanoseconds;

    return true;
}

[[nodiscard]] std::optional<mach_timebase_info_data_t> ReadMachTimebase(std::string& error) {
    mach_timebase_info_data_t timebase{};

    const kern_return_t result = ::mach_timebase_info(&timebase);

    if (result != KERN_SUCCESS || timebase.numer == 0 || timebase.denom == 0) {
        error = "failed to read Mach timebase";
        return std::nullopt;
    }

    return timebase;
}

[[nodiscard]] std::optional<std::uint64_t> ReadContinuousTimeNanoseconds(std::string& error) {
    const std::uint64_t continuous_ticks = ::mach_continuous_time();

    if (continuous_ticks == 0) {
        error = "mach_continuous_time returned zero";
        return std::nullopt;
    }

    const std::optional<mach_timebase_info_data_t> timebase = ReadMachTimebase(error);

    if (!timebase.has_value()) {
        return std::nullopt;
    }

    std::uint64_t nanoseconds = 0;

    if (!ConvertMachTicksToNanoseconds(continuous_ticks, *timebase, nanoseconds)) {
        error = "failed to convert continuous Mach time "
                "to nanoseconds";

        return std::nullopt;
    }

    return nanoseconds;
}

[[nodiscard]] std::optional<std::uint32_t> ReadLogicalCpuCount(std::string& error) {
    std::uint32_t logical_cpu_count = 0;

    std::size_t size = sizeof(logical_cpu_count);

    if (::sysctlbyname("hw.logicalcpu", &logical_cpu_count, &size, nullptr, 0) != 0) {
        error = ErrnoMessage("sysctlbyname(hw.logicalcpu) failed");

        return std::nullopt;
    }

    if (size != sizeof(logical_cpu_count) || logical_cpu_count == 0) {
        error = "hw.logicalcpu returned an invalid value";
        return std::nullopt;
    }

    return logical_cpu_count;
}

[[nodiscard]] std::optional<proc_taskallinfo> ReadProcessTaskAllInfo(const pid_t pid, std::string& error) {
    if (pid <= 0) {
        error = "pid must be greater than zero";
        return std::nullopt;
    }

    proc_taskallinfo task_all_info{};

    errno = 0;

    const int bytes_read =
        ::proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &task_all_info, static_cast<int>(sizeof(task_all_info)));

    const int expected_size = static_cast<int>(sizeof(task_all_info));

    if (bytes_read == expected_size) {
        return task_all_info;
    }

    if (bytes_read <= 0) {
        error = ErrnoMessage("proc_pidinfo(PROC_PIDTASKALLINFO) failed for pid " + std::to_string(pid));
    } else {
        error = "proc_pidinfo(PROC_PIDTASKALLINFO) returned incomplete data "
                "for pid "
                + std::to_string(pid);
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> BuildProcessIdentity(const proc_bsdinfo& bsd_info, std::string& error) {
    if (bsd_info.pbi_start_tvusec >= kMicrosecondsPerSecond) {
        error = "process start time contains an invalid microsecond field";
        return std::nullopt;
    }

    if (bsd_info.pbi_start_tvsec > std::numeric_limits<std::uint64_t>::max() / kMicrosecondsPerSecond) {
        error = "process start time conversion overflow";
        return std::nullopt;
    }

    const std::uint64_t seconds_part = bsd_info.pbi_start_tvsec * kMicrosecondsPerSecond;

    if (bsd_info.pbi_start_tvusec > std::numeric_limits<std::uint64_t>::max() - seconds_part) {
        error = "process start time conversion overflow";
        return std::nullopt;
    }

    return seconds_part + bsd_info.pbi_start_tvusec;
}

class MacosMetricsReader final : public PlatformMetricsReader {
public:
    std::optional<ProcessRawStats> ReadProcessRawStats(const pid_t pid, std::string& error) override {
        error.clear();

        const std::optional<proc_taskallinfo> task_all_info = ReadProcessTaskAllInfo(pid, error);

        if (!task_all_info.has_value()) {
            return std::nullopt;
        }

        const proc_taskinfo& task_info = task_all_info->ptinfo;

        const proc_bsdinfo& bsd_info = task_all_info->pbsd;

        if (task_info.pti_threadnum < 0) {
            error = "proc_taskallinfo returned a negative thread count";
            return std::nullopt;
        }

        std::uint64_t total_cpu_mach_ticks = 0;

        if (!AddWithoutOverflow(task_info.pti_total_user, task_info.pti_total_system, total_cpu_mach_ticks)) {
            error = "process CPU time overflow";
            return std::nullopt;
        }

        const std::optional<mach_timebase_info_data_t> timebase = ReadMachTimebase(error);

        if (!timebase.has_value()) {
            return std::nullopt;
        }

        std::uint64_t process_cpu_time_ns = 0;

        if (!ConvertMachTicksToNanoseconds(total_cpu_mach_ticks, *timebase, process_cpu_time_ns)) {
            error = "failed to convert Mach CPU time to nanoseconds";

            return std::nullopt;
        }

        const std::optional<std::uint64_t> process_identity = BuildProcessIdentity(bsd_info, error);

        if (!process_identity.has_value()) {
            return std::nullopt;
        }

        return ProcessRawStats{
            .process_cpu_time_ns = process_cpu_time_ns,
            .process_identity = *process_identity,
            .rss_bytes = task_info.pti_resident_size,
            .thread_count = static_cast<std::uint64_t>(task_info.pti_threadnum),
            .fd_count = static_cast<std::uint64_t>(bsd_info.pbi_nfiles),
        };
    }

    std::optional<SystemCapacitySnapshot> ReadSystemCapacity(std::string& error) override {
        error.clear();

        const std::optional<std::uint64_t> monotonic_time_ns = ReadContinuousTimeNanoseconds(error);

        if (!monotonic_time_ns.has_value()) {
            return std::nullopt;
        }

        const std::optional<std::uint32_t> logical_cpu_count = ReadLogicalCpuCount(error);

        if (!logical_cpu_count.has_value()) {
            return std::nullopt;
        }

        return SystemCapacitySnapshot{
            .monotonic_time_ns = *monotonic_time_ns,
            .logical_cpu_count = *logical_cpu_count,
        };
    }
};

} // namespace

std::unique_ptr<PlatformMetricsReader> CreatePlatformMetricsReader() {
    return std::make_unique<MacosMetricsReader>();
}

} // namespace aegis::agent