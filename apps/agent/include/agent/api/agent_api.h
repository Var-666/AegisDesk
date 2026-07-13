#pragma once

#include "agent/api/http_json.h"

#include <cstddef>
#include <string_view>

namespace aegis::agent {

class HealthMonitor;
class MetricsCollector;
class ProcessSupervisor;
class ServiceRegistry;

class AgentApi {
public:
    AgentApi(ServiceRegistry& registry, MetricsCollector& metrics_collector, HealthMonitor& health_monitor);

    [[nodiscard]] HttpResponse Handle(const HttpRequest& request);

private:
    [[nodiscard]] HttpResponse MakeServiceListResponse() const;

    [[nodiscard]] HttpResponse MakeStatusResponse(ProcessSupervisor& supervisor);

    [[nodiscard]] HttpResponse MakeActionResponse(ProcessSupervisor& supervisor, std::string_view action);

    [[nodiscard]] HttpResponse MakeLogsResponse(const ProcessSupervisor& supervisor, std::size_t tail);

    [[nodiscard]] HttpResponse MakeMetricsResponse(const ProcessSupervisor& supervisor) const;

    [[nodiscard]] HttpResponse MakeMetricsHistoryResponse(const ProcessSupervisor& supervisor, std::size_t limit) const;

    [[nodiscard]] HttpResponse MakeHealthResponse(const ProcessSupervisor& supervisor) const;

    [[nodiscard]] HttpResponse MakeServiceAlertsResponse(const ProcessSupervisor& supervisor,
                                                         bool include_resolved) const;

    [[nodiscard]] HttpResponse MakeAllAlertsResponse(bool include_resolved, std::size_t resolved_limit) const;

    [[nodiscard]] HttpResponse MakeActiveAlertsResponse() const;

    [[nodiscard]] HttpResponse MakeAcknowledgeAlertResponse(std::string_view alert_id) const;

    [[nodiscard]] HttpResponse MakeServiceRecoveryEventsResponse(const ProcessSupervisor& supervisor,
                                                                 std::size_t limit) const;

    [[nodiscard]] HttpResponse MakeAllRecoveryEventsResponse(std::size_t limit) const;

    [[nodiscard]] HttpResponse MakeMethodNotAllowed(std::string_view allow);

    ServiceRegistry& registry_;
    MetricsCollector& metrics_collector_;
    HealthMonitor& health_monitor_;
};

} // namespace aegis::agent