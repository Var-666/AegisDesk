//
// Created by Var on 2026/7/3.
//

#include "agent/log_reader.h"

#include <deque>
#include <fstream>

namespace aegis::agent {
std::vector<std::string> ReadLastLines(const std::filesystem::path& path, std::size_t max_lines, std::string& error) {
    error.clear();
    if (max_lines == 0 || !std::filesystem::exists(path)) {
        return {};
    }
    std::ifstream input(path);

    if (!input.is_open()) {
        error = "failed to open log file: " + path.string();
        return {};
    }

    std::deque<std::string> lines;
    std::string line;

    while (std::getline(input, line)) {
        if (lines.size() == max_lines) {
            lines.pop_front();
        }
        lines.push_back(std::move(line));
    }
    return {lines.begin(), lines.end()};
}
} // namespace aegis::agent
