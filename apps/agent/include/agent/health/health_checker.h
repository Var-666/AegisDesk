#pragma once

#include "agent/health/health_status.h"
#include "agent/service/process_supervisor.h"
#include "agent/service/service_definition.h"

#include <string_view>
#include <unordered_map>

namespace aegis::agent {
class HealthChecker {
public:
    [[nodiscard]] HealthStatus Check(const ServiceDefinition& definition, const ServiceStatus& status,
                                     UnixTimeMilliseconds checked_at_unix_ms);

    void ForgetService(std::string_view service_id);

    void Clear();

private:
    [[nodiscard]] HealthStatus CheckProcess(const ServiceDefinition& definition, const ServiceStatus& status,
                                            UnixTimeMilliseconds checked_at_unix_ms);

    [[nodiscard]] HealthStatus MakeUnknown(const ServiceDefinition& definition, std::string reason,
                                           UnixTimeMilliseconds checked_at_unix_ms);

private:
    std::unordered_map<std::string, std::uint32_t> consecutive_failures_;
};
} // namespace aegis::agent