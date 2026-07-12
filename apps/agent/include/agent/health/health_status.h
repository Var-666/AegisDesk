#pragma once

#include "agent/metrics/service_metrics.h"
#include "agent/service/service_id.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aegis::agent {

enum class HealthCheckType {
    kProcess,
    kHttp,
    kTcp,
    kCommand,
};

enum class HealthState {
    kHealthy,
    kUnhealthy,
    kUnknown,
};

[[nodiscard]] inline std::string_view ToString(const HealthCheckType type) noexcept {
    switch (type) {
        case HealthCheckType::kProcess:
            return "process";
        case HealthCheckType::kHttp:
            return "http";
        case HealthCheckType::kTcp:
            return "tcp";
        case HealthCheckType::kCommand:
            return "command";
    }

    return "unknown";
}

[[nodiscard]] inline std::string_view ToString(const HealthState state) noexcept {
    switch (state) {
        case HealthState::kHealthy:
            return "healthy";
        case HealthState::kUnhealthy:
            return "unhealthy";
        case HealthState::kUnknown:
            return "unknown";
    }

    return "unknown";
}

struct HealthCheckDefinition {
    bool enabled{true};

    HealthCheckType type{HealthCheckType::kProcess};

    std::uint32_t interval_seconds{5};
    std::uint32_t timeout_milliseconds{1000};
    std::uint32_t failure_threshold{3};

    std::string endpoint;

    std::string host;
    std::uint16_t port{0};

    std::string command;

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!enabled) {
            return true;
        }

        if (interval_seconds == 0 || interval_seconds > 3600) {
            return false;
        }

        if (timeout_milliseconds == 0 || timeout_milliseconds > 60000) {
            return false;
        }

        if (failure_threshold == 0 || failure_threshold > 100) {
            return false;
        }

        switch (type) {
            case HealthCheckType::kProcess:
                return true;

            case HealthCheckType::kHttp:
                return !endpoint.empty();

            case HealthCheckType::kTcp:
                return !host.empty() && port > 0;

            case HealthCheckType::kCommand:
                return !command.empty();
        }

        return false;
    }
};

struct HealthStatus {
    std::string service_id;
    HealthState state{HealthState::kUnknown};
    std::string reason;

    std::uint32_t consecutive_failures{0};
    UnixTimeMilliseconds checked_at_unix_ms{0};

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!IsValidServiceId(service_id)) {
            return false;
        }

        if (checked_at_unix_ms < 0) {
            return false;
        }

        switch (state) {
            case HealthState::kHealthy:
                return true;
            case HealthState::kUnhealthy:
                return !reason.empty();
            case HealthState::kUnknown:
                return true;
        }

        return false;
    }
};

} // namespace aegis::agent
