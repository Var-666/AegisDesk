#pragma once

#include "agent/metrics/service_metrics.h"
#include "alert_rule.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aegis::agent {

enum class AlertEventState {
    kActive,
    kResolved,
};

[[nodiscard]] inline std::string_view ToString(const AlertEventState state) noexcept {
    switch (state) {
        case AlertEventState::kActive:
            return "active";
        case AlertEventState::kResolved:
            return "resolved";
    }

    return "unknown";
}

[[nodiscard]] inline bool IsValidAlertEventId(const std::string_view id) noexcept {
    if (id.empty() || id.size() > 160) {
        return false;
    }

    for (const char ch : id) {
        const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'
                           || ch == '-' || ch == '.' || ch == ':';

        if (!valid) {
            return false;
        }
    }

    return true;
}

struct AlertEvent {
    std::string id;
    std::string service_id;
    std::string rule_id;

    AlertSeverity severity{AlertSeverity::kWarning};

    AlertEventState state{AlertEventState::kActive};

    std::string message;

    UnixTimeMilliseconds first_triggered_at_unix_ms{0};
    UnixTimeMilliseconds last_triggered_at_unix_ms{0};

    std::optional<UnixTimeMilliseconds> resolved_at_unix_ms;

    std::uint32_t trigger_count{1};

    bool acknowledged{false};

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!IsValidAlertEventId(id)) {
            return false;
        }

        if (!IsValidServiceId(service_id)) {
            return false;
        }

        if (!IsValidAlertRuleId(rule_id)) {
            return false;
        }

        if (message.empty()) {
            return false;
        }

        if (first_triggered_at_unix_ms < 0 || last_triggered_at_unix_ms < 0) {
            return false;
        }

        if (last_triggered_at_unix_ms < first_triggered_at_unix_ms) {
            return false;
        }

        if (trigger_count == 0) {
            return false;
        }

        if (state == AlertEventState::kActive) {
            return !resolved_at_unix_ms.has_value();
        }

        if (state == AlertEventState::kResolved) {
            if (!resolved_at_unix_ms.has_value()) {
                return false;
            }

            return *resolved_at_unix_ms >= last_triggered_at_unix_ms;
        }

        return false;
    }
};

} // namespace aegis::agent
