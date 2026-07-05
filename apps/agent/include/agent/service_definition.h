#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace aegis::agent {
constexpr bool IsAllowServiceIdChar(const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
           || (character >= '0' && character <= '9') || character == '_' || character == '-';
}

inline bool IsValidServiceId(const std::string_view service_id) {
    constexpr std::size_t kMaxServiceIdLength = 64;

    if (service_id.empty() || service_id.size() > kMaxServiceIdLength) {
        return false;
    }

    for (const char character : service_id) {
        if (!IsAllowServiceIdChar(character)) {
            return false;
        }
    }

    return true;
}

struct ServiceDefinition {
    std::string id;
    std::string display_name;
    std::filesystem::path executable;
    std::filesystem::path work_dir;
    std::vector<std::string> args;
    std::filesystem::path log_path;
    bool auto_start{false};

    bool IsStructurallyValid() const noexcept {
        return IsValidServiceId(id) && !display_name.empty() && !executable.empty() && !work_dir.empty()
               && !log_path.empty();
    }
};
} // namespace aegis::agent