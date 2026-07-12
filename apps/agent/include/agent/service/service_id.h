#pragma once

#include <string_view>

namespace aegis::agent {

[[nodiscard]] inline bool IsValidServiceId(const std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) {
        return false;
    }

    for (const char ch : id) {
        const bool valid =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';

        if (!valid) {
            return false;
        }
    }

    return true;
}

} // namespace aegis::agent