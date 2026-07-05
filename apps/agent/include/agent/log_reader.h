#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace aegis::agent {
std::vector<std::string> ReadLastLines(const std::filesystem::path& path, std::size_t max_lines, std::string& error);
}
