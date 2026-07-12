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
    [[nodiscard]] HttpResponse MakeServiceListResponse();

    [[nodiscard]] HttpResponse MakeStatusResponse(ProcessSupervisor& supervisor);

    [[nodiscard]] HttpResponse MakeActionResponse(ProcessSupervisor& supervisor, std::string_view action);

    [[nodiscard]] HttpResponse MakeLogsResponse(ProcessSupervisor& supervisor, std::size_t tail);

    [[nodiscard]] HttpResponse MakeMetricsResponse(ProcessSupervisor& supervisor);

    [[nodiscard]] HttpResponse MakeMetricsHistoryResponse(ProcessSupervisor& supervisor, std::size_t limit);

    [[nodiscard]] HttpResponse MakeHealthResponse(ProcessSupervisor& supervisor);

    [[nodiscard]] HttpResponse MakeServiceAlertsResponse(ProcessSupervisor& supervisor, bool include_resolved);

    [[nodiscard]] HttpResponse MakeAllAlertsResponse(bool include_resolved, std::size_t resolved_limit);

    [[nodiscard]] HttpResponse MakeActiveAlertsResponse();

    [[nodiscard]] HttpResponse MakeAcknowledgeAlertResponse(std::string_view alert_id);

    [[nodiscard]] HttpResponse MakeServiceRecoveryEventsResponse(ProcessSupervisor& supervisor, std::size_t limit);

    [[nodiscard]] HttpResponse MakeAllRecoveryEventsResponse(std::size_t limit);

    [[nodiscard]] HttpResponse MakeMethodNotAllowed(std::string_view allow);

    ServiceRegistry& registry_;
    MetricsCollector& metrics_collector_;
    HealthMonitor& health_monitor_;
};

} // namespace aegis::agent