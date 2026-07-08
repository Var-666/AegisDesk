#include "agent/platform_metrics_reader.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace aegis::agent {
namespace {

constexpr std::uint64_t kBytesPerKilobyte = 1024;

constexpr std::size_t kProcStatUtimeIndex = 11;
constexpr std::size_t kProcStatStimeIndex = 12;
constexpr std::size_t kProcStatStartTimeIndex = 19;

constexpr std::size_t kMinimumProcStatFieldCount = kProcStatStartTimeIndex + 1;

[[nodiscard]] std::filesystem::path MakeProcPath(const pid_t pid, const std::string_view file_name) {
    return std::filesystem::path("/proc") / std::to_string(pid) / std::string(file_name);
}

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }

    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }

    return value;
}

[[nodiscard]] std::vector<std::string_view> SplitWhitespace(const std::string_view value) {
    std::vector<std::string_view> tokens;

    std::size_t position = 0;

    while (position < value.size()) {
        while (position < value.size() && (value[position] == ' ' || value[position] == '\t')) {
            ++position;
        }

        if (position == value.size()) {
            break;
        }

        const std::size_t start = position;

        while (position < value.size() && value[position] != ' ' && value[position] != '\t') {
            ++position;
        }

        tokens.push_back(value.substr(start, position - start));
    }

    return tokens;
}

template <typename Integer> [[nodiscard]] bool ParseUnsigned(const std::string_view value, Integer& parsed) {
    static_assert(std::is_unsigned_v<Integer>);

    if (value.empty()) {
        return false;
    }

    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);

    return error == std::errc{} && end == value.data() + value.size();
}

[[nodiscard]] bool AddWithoutOverflow(const std::uint64_t left, const std::uint64_t right,
                                      std::uint64_t& result) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }

    result = left + right;
    return true;
}

[[nodiscard]] bool ConvertTicksToNanoseconds(const std::uint64_t ticks, const long ticks_per_second,
                                             std::uint64_t& nanoseconds) {
    constexpr std::uint64_t kNanosecondsPerSecond = 1000ULL * 1000ULL * 1000ULL;

    if (ticks_per_second <= 0) {
        return false;
    }

    const std::uint64_t ticks_per_second_u64 = static_cast<std::uint64_t>(ticks_per_second);

    const std::uint64_t whole_seconds = ticks / ticks_per_second_u64;

    const std::uint64_t remaining_ticks = ticks % ticks_per_second_u64;

    if (whole_seconds > std::numeric_limits<std::uint64_t>::max() / kNanosecondsPerSecond) {
        return false;
    }

    const std::uint64_t whole_nanoseconds = whole_seconds * kNanosecondsPerSecond;

    const long double fractional_nanoseconds = static_cast<long double>(remaining_ticks)
                                               * static_cast<long double>(kNanosecondsPerSecond)
                                               / static_cast<long double>(ticks_per_second_u64);

    if (!std::isfinite(fractional_nanoseconds) || fractional_nanoseconds < 0.0L
        || fractional_nanoseconds
               > static_cast<long double>(std::numeric_limits<std::uint64_t>::max() - whole_nanoseconds)) {
        return false;
    }

    nanoseconds = whole_nanoseconds + static_cast<std::uint64_t>(fractional_nanoseconds);

    return true;
}

[[nodiscard]] std::optional<std::uint64_t> ReadMonotonicTimeNanoseconds(std::string& error) {
    constexpr std::uint64_t kNanosecondsPerSecond = 1000ULL * 1000ULL * 1000ULL;

    timespec time_value{};

    if (::clock_gettime(CLOCK_MONOTONIC, &time_value) != 0) {
        error = "failed to read CLOCK_MONOTONIC";
        return std::nullopt;
    }

    if (time_value.tv_sec < 0 || time_value.tv_nsec < 0) {
        error = "CLOCK_MONOTONIC returned a negative value";
        return std::nullopt;
    }

    const std::uint64_t seconds = static_cast<std::uint64_t>(time_value.tv_sec);

    const std::uint64_t nanoseconds = static_cast<std::uint64_t>(time_value.tv_nsec);

    if (seconds > std::numeric_limits<std::uint64_t>::max() / kNanosecondsPerSecond) {
        error = "CLOCK_MONOTONIC nanosecond conversion overflow";
        return std::nullopt;
    }

    const std::uint64_t whole_nanoseconds = seconds * kNanosecondsPerSecond;

    if (nanoseconds > std::numeric_limits<std::uint64_t>::max() - whole_nanoseconds) {
        error = "CLOCK_MONOTONIC nanosecond conversion overflow";
        return std::nullopt;
    }

    return whole_nanoseconds + nanoseconds;
}

[[nodiscard]] std::optional<std::uint32_t> ReadLogicalCpuCount(std::string& error) {
    const long logical_cpu_count = ::sysconf(_SC_NPROCESSORS_ONLN);

    if (logical_cpu_count <= 0 || logical_cpu_count > static_cast<long>(std::numeric_limits<std::uint32_t>::max())) {
        error = "failed to read online logical CPU count";
        return std::nullopt;
    }

    return static_cast<std::uint32_t>(logical_cpu_count);
}

struct ProcStatCpuFields {
    std::uint64_t process_cpu_ticks{0};
    std::uint64_t start_time_ticks{0};
};

[[nodiscard]] std::optional<ProcStatCpuFields> ReadProcStatCpuFields(const pid_t pid, std::string& error) {
    const std::filesystem::path stat_path = MakeProcPath(pid, "stat");

    std::ifstream input(stat_path);

    if (!input.is_open()) {
        error = "failed to open " + stat_path.string();
        return std::nullopt;
    }

    std::string line;

    if (!std::getline(input, line)) {
        error = "failed to read " + stat_path.string();
        return std::nullopt;
    }

    const std::size_t right_parenthesis = line.rfind(')');

    if (right_parenthesis == std::string::npos || right_parenthesis + 1 >= line.size()) {
        error = "invalid format in " + stat_path.string() + ": missing process name terminator";

        return std::nullopt;
    }

    const std::string_view fields_text = Trim(std::string_view(line).substr(right_parenthesis + 1));

    const std::vector<std::string_view> fields = SplitWhitespace(fields_text);

    if (fields.size() < kMinimumProcStatFieldCount) {
        error = "invalid format in " + stat_path.string() + ": too few fields";
        return std::nullopt;
    }

    std::uint64_t utime = 0;
    std::uint64_t stime = 0;
    std::uint64_t start_time = 0;

    if (!ParseUnsigned(fields[kProcStatUtimeIndex], utime) || !ParseUnsigned(fields[kProcStatStimeIndex], stime)
        || !ParseUnsigned(fields[kProcStatStartTimeIndex], start_time)) {
        error = "invalid numeric CPU fields in " + stat_path.string();
        return std::nullopt;
    }

    std::uint64_t process_cpu_ticks = 0;

    if (!AddWithoutOverflow(utime, stime, process_cpu_ticks)) {
        error = "process CPU tick overflow in " + stat_path.string();
        return std::nullopt;
    }

    return ProcStatCpuFields{
        .process_cpu_ticks = process_cpu_ticks,
        .start_time_ticks = start_time,
    };
}

struct ProcStatusFields {
    std::uint64_t rss_bytes{0};
    std::uint64_t thread_count{0};
};

[[nodiscard]] std::optional<ProcStatusFields> ReadProcStatusFields(const pid_t pid, std::string& error) {
    const std::filesystem::path status_path = MakeProcPath(pid, "status");

    std::ifstream input(status_path);

    if (!input.is_open()) {
        error = "failed to open " + status_path.string();
        return std::nullopt;
    }

    bool found_rss = false;
    bool found_threads = false;

    std::uint64_t rss_kilobytes = 0;
    std::uint64_t thread_count = 0;

    std::string line;

    while (std::getline(input, line)) {
        const std::string_view view = line;

        if (view.starts_with("VmRSS:")) {
            const std::vector<std::string_view> tokens =
                SplitWhitespace(view.substr(std::string_view("VmRSS:").size()));

            if (tokens.empty() || !ParseUnsigned(tokens[0], rss_kilobytes)) {
                error = "invalid VmRSS field in " + status_path.string();

                return std::nullopt;
            }

            found_rss = true;
            continue;
        }

        if (view.starts_with("Threads:")) {
            const std::vector<std::string_view> tokens =
                SplitWhitespace(view.substr(std::string_view("Threads:").size()));

            if (tokens.empty() || !ParseUnsigned(tokens[0], thread_count)) {
                error = "invalid Threads field in " + status_path.string();

                return std::nullopt;
            }

            found_threads = true;
        }
    }

    if (!found_rss || !found_threads) {
        error = "missing VmRSS or Threads field in " + status_path.string();

        return std::nullopt;
    }

    if (rss_kilobytes > std::numeric_limits<std::uint64_t>::max() / kBytesPerKilobyte) {
        error = "VmRSS byte conversion overflow in " + status_path.string();

        return std::nullopt;
    }

    return ProcStatusFields{
        .rss_bytes = rss_kilobytes * kBytesPerKilobyte,
        .thread_count = thread_count,
    };
}

[[nodiscard]] std::optional<std::uint64_t> CountOpenFileDescriptors(const pid_t pid, std::string& error) {
    const std::filesystem::path fd_path = MakeProcPath(pid, "fd");

    std::error_code directory_error;

    std::filesystem::directory_iterator iterator(fd_path, directory_error);

    if (directory_error) {
        error = "failed to open " + fd_path.string() + ": " + directory_error.message();

        return std::nullopt;
    }

    std::uint64_t count = 0;

    const std::filesystem::directory_iterator end;

    while (iterator != end) {
        if (count == std::numeric_limits<std::uint64_t>::max()) {
            error = "file descriptor count overflow in " + fd_path.string();
            return std::nullopt;
        }

        ++count;

        iterator.increment(directory_error);

        if (directory_error) {
            error = "failed to iterate " + fd_path.string() + ": " + directory_error.message();
            return std::nullopt;
        }
    }

    return count;
}

class LinuxProcfsMetricsReader final : public PlatformMetricsReader {
public:
    std::optional<ProcessRawStats> ReadProcessRawStats(const pid_t pid, std::string& error) override {
        error.clear();

        if (pid <= 0) {
            error = "pid must be greater than zero";
            return std::nullopt;
        }

        const std::optional<ProcStatCpuFields> stat_fields = ReadProcStatCpuFields(pid, error);

        if (!stat_fields.has_value()) {
            return std::nullopt;
        }

        const std::optional<ProcStatusFields> status_fields = ReadProcStatusFields(pid, error);

        if (!status_fields.has_value()) {
            return std::nullopt;
        }

        const std::optional<std::uint64_t> fd_count = CountOpenFileDescriptors(pid, error);

        if (!fd_count.has_value()) {
            return std::nullopt;
        }

        const long ticks_per_second = ::sysconf(_SC_CLK_TCK);

        std::uint64_t process_cpu_time_ns = 0;

        if (!ConvertTicksToNanoseconds(stat_fields->process_cpu_ticks, ticks_per_second, process_cpu_time_ns)) {
            error = "failed to convert Linux CPU ticks to nanoseconds";

            return std::nullopt;
        }

        return ProcessRawStats{
            .process_cpu_time_ns = process_cpu_time_ns,
            .process_identity = stat_fields->start_time_ticks,
            .rss_bytes = status_fields->rss_bytes,
            .thread_count = status_fields->thread_count,
            .fd_count = *fd_count,
        };
    }

    std::optional<SystemCapacitySnapshot> ReadSystemCapacity(std::string& error) override {
        error.clear();

        const std::optional<std::uint64_t> monotonic_time_ns = ReadMonotonicTimeNanoseconds(error);

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
    return std::make_unique<LinuxProcfsMetricsReader>();
}

} // namespace aegis::agent