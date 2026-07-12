#include "agent/health/health_checker.h"

#include <limits>
#include <string>

namespace aegis::agent {
HealthStatus HealthChecker::Check(const ServiceDefinition& definition, const ServiceStatus& status,
                                  const UnixTimeMilliseconds checked_at_unix_ms) {
    if (!definition.health_check.enabled) {
        consecutive_failures_.erase(definition.id);

        return HealthStatus{
            .service_id = definition.id,
            .state = HealthState::kUnknown,
            .reason = "health check is disabled",
            .consecutive_failures = 0,
            .checked_at_unix_ms = checked_at_unix_ms,
        };
    }

    if (!definition.health_check.IsStructurallyValid()) {
        consecutive_failures_.erase(definition.id);

        return HealthStatus{
            .service_id = definition.id,
            .state = HealthState::kUnknown,
            .reason = "health check definition is invalid",
            .consecutive_failures = 0,
            .checked_at_unix_ms = checked_at_unix_ms,
        };
    }

    switch (definition.health_check.type) {
        case HealthCheckType::kProcess:
            return CheckProcess(definition, status, checked_at_unix_ms);
        case HealthCheckType::kHttp:
            return MakeUnknown(definition, "http health check is not implemented yet", checked_at_unix_ms);
        case HealthCheckType::kTcp:
            return MakeUnknown(definition, "tcp health check is not implemented yet", checked_at_unix_ms);
        case HealthCheckType::kCommand:
            return MakeUnknown(definition, "command health check is not implemented yet", checked_at_unix_ms);
    }
    return MakeUnknown(definition, "unknown health check type", checked_at_unix_ms);
}
void HealthChecker::ForgetService(const std::string_view service_id) {
    consecutive_failures_.erase(std::string(service_id));
}
void HealthChecker::Clear() {
    consecutive_failures_.clear();
}
HealthStatus HealthChecker::CheckProcess(const ServiceDefinition& definition, const ServiceStatus& status,
                                         const UnixTimeMilliseconds checked_at_unix_ms) {

    if (status.state == ServiceState::kRunning && status.pid > 0) {
        consecutive_failures_.erase(definition.id);

        return HealthStatus{
            .service_id = definition.id,
            .state = HealthState::kHealthy,
            .reason = "process is running",
            .consecutive_failures = 0,
            .checked_at_unix_ms = checked_at_unix_ms,
        };
    }

    std::uint32_t& failure_count = consecutive_failures_[definition.id];

    if (failure_count < std::numeric_limits<std::uint32_t>::max()) {
        ++failure_count;
    }

    if (failure_count < std::max<std::uint32_t>(1, definition.health_check.failure_threshold)) {
        return HealthStatus{
            .service_id = definition.id,
            .state = HealthState::kUnknown,
            .reason = "process is not running, waiting for failure threshold",
            .consecutive_failures = failure_count,
            .checked_at_unix_ms = checked_at_unix_ms,
        };
    }

    return HealthStatus{
        .service_id = definition.id,
        .state = HealthState::kUnhealthy,
        .reason = "process is not running",
        .consecutive_failures = failure_count,
        .checked_at_unix_ms = checked_at_unix_ms,
    };
}
HealthStatus HealthChecker::MakeUnknown(const ServiceDefinition& definition, std::string reason,
                                        const UnixTimeMilliseconds checked_at_unix_ms) {
    consecutive_failures_.erase(definition.id);

    return HealthStatus{
        .service_id = definition.id,
        .state = HealthState::kUnknown,
        .reason = std::move(reason),
        .consecutive_failures = 0,
        .checked_at_unix_ms = checked_at_unix_ms,
    };
}

} // namespace aegis::agent