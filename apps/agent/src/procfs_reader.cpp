#include "agent/procfs_reader.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace aegis::agent {
namespace {
constexpr std::uint64_t kBytesPerKilobyte = 1024;

constexpr std::uint64_t kProcStatUtimeIndex = 11;
constexpr std::uint64_t kProcStatStimeIndex = 12;
constexpr std::uint64_t kProcStatStartTimeIndex = 19;

constexpr std::size_t kMinimumProcStatFieldCount = kProcStatStartTimeIndex + 1;

// 构造 /proc 路径
[[nodiscard]] std::filesystem::path MakeProcPath(const pid_t pid, const std::string_view file_name) {
    return std::filesystem::path("/proc") / std::to_string(pid) / std::string(file_name);
}

// 删除字符串首尾空白字符
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

// 按空格或 Tab 分割字符串
[[nodiscard]] std::vector<std::string_view> SplitWhitespace(const std::string_view value) {
    std::vector<std::string_view> tokens;

    std::size_t position = 0;

    while (position < value.size()) {
        while (position < value.size() && (value.at(position) == ' ' || value.at(position) == '\t')) {
            position++;
        }

        if (position == value.size()) {
            break;
        }

        const std::size_t start = position;

        while (position < value.size() && value[position] != ' ' && value[position] != '\t') {
            position++;
        }

        tokens.push_back(value.substr(start, position - start));
    }
    return tokens;
}

// 把文本数字安全转换为整数
template <typename Integer> [[nodiscard]] bool ParseUnsigned(const std::string_view value, Integer& parsed) {
    static_assert(std::is_integral_v<Integer>);

    if (value.empty()) {
        return false;
    }

    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);

    return error == std::errc{} && end == value.data() + value.size();
}

// 安全地计算两个无符号整数之和
[[nodiscard]] bool AddWithoutOverflow(const std::uint64_t left, const std::uint64_t right,
                                      std::uint64_t& result) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }

    result = left + right;
    return true;
}

struct ProcStatCpuFields {
    std::uint64_t process_cpu_ticks{0};
    std::uint64_t start_time_ticks{0};
};

// 读取 /proc/<pid>/stat
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

// 读取 /proc/<pid>/status
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

// 统计打开文件描述符数量
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
} // namespace

std::optional<ProcessRawStats> ReadProcessRawStats(const pid_t pid, std::string& error) {
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

    return ProcessRawStats{
        .process_cpu_ticks = stat_fields->process_cpu_ticks,
        .start_time_ticks = stat_fields->start_time_ticks,
        .rss_bytes = status_fields->rss_bytes,
        .thread_count = status_fields->thread_count,
        .fd_count = *fd_count,
    };
}

std::optional<SystemCpuStats> ReadSystemCpuStats(std::string& error) {
    error.clear();

    constexpr std::string_view kProcStatPath{"/proc/stat"};

    std::ifstream input{std::string(kProcStatPath)};

    if (!input.is_open()) {
        error = "failed to open " + std::string(kProcStatPath);
        return std::nullopt;
    }

    std::string line;

    while (std::getline(input, line)) {
        const std::string_view view = line;

        if (!view.starts_with("cpu ")) {
            continue;
        }

        const std::vector<std::string_view> fields = SplitWhitespace(view.substr(std::string_view("cpu").size()));

        // 至少应有 user、nice、system、idle 四项。
        if (fields.size() < 4) {
            error = "invalid format in /proc/stat: "
                    "too few CPU fields";
            return std::nullopt;
        }

        constexpr std::size_t kMaxCpuFields = 8;

        const std::size_t field_count = std::min(fields.size(), kMaxCpuFields);

        std::uint64_t total_cpu_ticks = 0;

        for (std::size_t index = 0; index < field_count; ++index) {
            std::uint64_t value = 0;

            if (!ParseUnsigned(fields[index], value)) {
                error = "invalid numeric CPU field in /proc/stat";
                return std::nullopt;
            }

            std::uint64_t next_total = 0;

            if (!AddWithoutOverflow(total_cpu_ticks, value, next_total)) {
                error = "system CPU tick overflow in /proc/stat";
                return std::nullopt;
            }

            total_cpu_ticks = next_total;
        }

        return SystemCpuStats{
            .total_cpu_ticks = total_cpu_ticks,
        };
    }

    error = "cpu summary line not found in /proc/stat";
    return std::nullopt;
}

} // namespace aegis::agent