#pragma once

#include <optional>
#include <string_view>

namespace aegis::agent {
enum class DesiredState {
    kStopped,
    kRunning,
};

[[nodiscard]] inline std::string_view ToString(
    const DesiredState state
) noexcept {
    switch (state) {
        case DesiredState::kStopped:
            return "stopped";

        case DesiredState::kRunning:
            return "running";
    }

    return "unknown";
}

[[nodiscard]] inline std::optional<DesiredState> ParseDesiredState(
    const std::string_view text
) noexcept {
    if (text == "stopped") {
        return DesiredState::kStopped;
    }

    if (text == "running") {
        return DesiredState::kRunning;
    }

    return std::nullopt;
}

[[nodiscard]] inline bool IsDesiredRunning(
    const DesiredState state
) noexcept {
    return state == DesiredState::kRunning;
}

[[nodiscard]] inline bool IsDesiredStopped(
    const DesiredState state
) noexcept {
    return state == DesiredState::kStopped;
}

} // namespace aegis::agent