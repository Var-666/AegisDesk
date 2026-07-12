#pragma once

#include "agent/health/health_status.h"
#include "agent/service/service_id.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aegis::agent {

enum class AlertMetric {
    kCpuPercent,
    kRssBytes,
    kThreadCount,
    kFdCount,
    kHealthState,
};

enum class AlertOperator {
    kGreaterThan,
    kGreaterThanOrEqual,
    kLessThan,
    kLessThanOrEqual,
    kEqual,
    kNotEqual,
};

enum class AlertSeverity {
    kInfo,
    kWarning,
    kCritical,
};

[[nodiscard]] inline bool IsValidAlertRuleId(const std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) {
        return false;
    }

    for (const char ch : id) {
        const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'
                           || ch == '-' || ch == '.';

        if (!valid) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] inline std::string_view ToString(const AlertMetric metric) noexcept {
    switch (metric) {
        case AlertMetric::kCpuPercent:
            return "cpu_percent";
        case AlertMetric::kRssBytes:
            return "rss_bytes";
        case AlertMetric::kThreadCount:
            return "thread_count";
        case AlertMetric::kFdCount:
            return "fd_count";
        case AlertMetric::kHealthState:
            return "health_state";
    }

    return "unknown";
}

[[nodiscard]] inline std::string_view ToString(const AlertOperator op) noexcept {
    switch (op) {
        case AlertOperator::kGreaterThan:
            return ">";
        case AlertOperator::kGreaterThanOrEqual:
            return ">=";
        case AlertOperator::kLessThan:
            return "<";
        case AlertOperator::kLessThanOrEqual:
            return "<=";
        case AlertOperator::kEqual:
            return "==";
        case AlertOperator::kNotEqual:
            return "!=";
    }

    return "unknown";
}

[[nodiscard]] inline std::string_view ToString(const AlertSeverity severity) noexcept {
    switch (severity) {
        case AlertSeverity::kInfo:
            return "info";
        case AlertSeverity::kWarning:
            return "warning";
        case AlertSeverity::kCritical:
            return "critical";
    }

    return "unknown";
}

struct AlertRule {
    std::string id;
    std::string service_id;

    AlertMetric metric{AlertMetric::kCpuPercent};

    AlertOperator op{AlertOperator::kGreaterThan};

    std::optional<double> numeric_threshold;

    std::optional<HealthState> health_state_threshold;

    std::uint32_t duration_seconds{0};

    AlertSeverity severity{AlertSeverity::kWarning};

    bool enabled{true};

    std::string description;

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!IsValidAlertRuleId(id)) {
            return false;
        }

        if (!IsValidServiceId(service_id)) {
            return false;
        }

        if (duration_seconds > 86400) {
            return false;
        }

        if (metric == AlertMetric::kHealthState) {
            if (!health_state_threshold.has_value()) {
                return false;
            }

            if (numeric_threshold.has_value()) {
                return false;
            }

            return op == AlertOperator::kEqual || op == AlertOperator::kNotEqual;
        }

        if (!numeric_threshold.has_value()) {
            return false;
        }

        if (health_state_threshold.has_value()) {
            return false;
        }

        if (!std::isfinite(*numeric_threshold)) {
            return false;
        }

        if (metric == AlertMetric::kCpuPercent) {
            return *numeric_threshold >= 0.0 && *numeric_threshold <= 100.0;
        }

        return *numeric_threshold >= 0.0;
    }
};

} // namespace aegis::agent
