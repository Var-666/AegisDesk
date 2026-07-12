#include "agent/service/log_reader.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aegis::agent {
namespace {

constexpr std::uintmax_t kInitialReadWindowBytes = 64 * 1024;

std::vector<std::string> KeepLastLines(const std::string_view content, const std::size_t max_lines) {
    std::deque<std::string> lines;

    std::istringstream input{std::string(content)};
    std::string line;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (lines.size() == max_lines) {
            lines.pop_front();
        }

        lines.push_back(std::move(line));
    }

    return {lines.begin(), lines.end()};
}

} // namespace

std::vector<std::string> ReadLastLines(const std::filesystem::path& path, const std::size_t max_lines,
                                       std::string& error) {
    error.clear();

    if (max_lines == 0 || !std::filesystem::exists(path)) {
        return {};
    }

    std::error_code file_size_error;

    const std::uintmax_t file_size = std::filesystem::file_size(path, file_size_error);

    if (file_size_error) {
        error = "failed to get log file size: " + file_size_error.message();
        return {};
    }

    if (file_size == 0) {
        return {};
    }

    std::ifstream input(path, std::ios::binary);

    if (!input.is_open()) {
        error = "failed to open log file: " + path.string();
        return {};
    }

    std::uintmax_t window_size = std::min(file_size, kInitialReadWindowBytes);

    while (true) {
        const std::uintmax_t start_offset = file_size - window_size;

        input.clear();

        input.seekg(static_cast<std::streamoff>(start_offset), std::ios::beg);

        if (!input.good()) {
            error = "failed to seek log file: " + path.string();
            return {};
        }

        std::string buffer(static_cast<std::size_t>(window_size), '\0');

        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));

        buffer.resize(static_cast<std::size_t>(input.gcount()));

        std::string_view content(buffer);

        // 从文件中间开始读取时，首行可能是残缺日志。
        // 丢弃至第一个换行符之前的内容。
        if (start_offset > 0) {
            const std::size_t first_newline = content.find('\n');

            if (first_newline == std::string_view::npos) {
                // 当前窗口没有完整的一行，扩大读取窗口。
                if (window_size == file_size) {
                    return {};
                }

                window_size = std::min(file_size, window_size * 2);

                continue;
            }

            content.remove_prefix(first_newline + 1);
        }

        const std::vector<std::string> lines = KeepLastLines(content, max_lines);

        // 已经找到足够日志；或文件本身比当前窗口小。
        if (lines.size() >= max_lines || start_offset == 0) {
            return lines;
        }

        window_size = std::min(file_size, window_size * 2);
    }
}

} // namespace aegis::agent