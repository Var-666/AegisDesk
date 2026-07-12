#pragma once

#include "agent/metrics/service_metrics.h"
#include "agent/service/service_id.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aegis::agent {

enum class RecoveryAction {
    kNone,
    kRestart,
};

enum class RecoveryEventType {
    kRestartStarted,
    kRestartSucceeded,
    kRestartFailed,
    kRestartSuppressed,
};

[[nodiscard]] inline std::string_view ToString(const RecoveryAction action) noexcept {
    switch (action) {
        case RecoveryAction::kNone:
            return "none";
        case RecoveryAction::kRestart:
            return "restart";
    }

    return "unknown";
}

[[nodiscard]] inline std::string_view ToString(const RecoveryEventType type) noexcept {
    switch (type) {
        case RecoveryEventType::kRestartStarted:
            return "restart_started";
        case RecoveryEventType::kRestartSucceeded:
            return "restart_succeeded";
        case RecoveryEventType::kRestartFailed:
            return "restart_failed";
        case RecoveryEventType::kRestartSuppressed:
            return "restart_suppressed";
    }

    return "unknown";
}

struct RecoveryPolicy {
    bool enabled{false};

    bool restart_on_unhealthy{false};

    bool restart_on_critical_alert{false};

    std::uint32_t max_restarts{3};
    std::uint32_t window_seconds{300};

    std::uint32_t backoff_seconds{10};

    std::uint32_t startup_grace_seconds{10};

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!enabled) {
            return true;
        }

        if (!restart_on_unhealthy && !restart_on_critical_alert) {
            return false;
        }

        if (max_restarts == 0 || max_restarts > 100) {
            return false;
        }

        if (window_seconds == 0 || window_seconds > 86400) {
            return false;
        }

        if (backoff_seconds > 3600) {
            return false;
        }

        if (startup_grace_seconds > 3600) {
            return false;
        }

        return true;
    }
};

struct RecoveryEvent {
    std::string service_id;

    RecoveryEventType type{RecoveryEventType::kRestartStarted};

    UnixTimeMilliseconds occurred_at_unix_ms{0};

    std::string reason;

    std::optional<std::string> alert_event_id;

    std::uint32_t restart_count_in_window{0};

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!IsValidServiceId(service_id)) {
            return false;
        }

        if (occurred_at_unix_ms < 0) {
            return false;
        }

        if (reason.empty()) {
            return false;
        }

        return true;
    }
};

} // namespace aegis::agent
