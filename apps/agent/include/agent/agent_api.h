#pragma once

#include "agent/http_json.h"

#include <filesystem>
#include <string_view>

namespace aegis::agent {

class MetricsCollector;
class ProcessSupervisor;
class ServiceRegistry;

class AgentApi {
public:
    explicit AgentApi(ServiceRegistry& registry, MetricsCollector& metrics_collector);

    [[nodiscard]] HttpResponse Handle(const HttpRequest& request);

private:
    [[nodiscard]] HttpResponse MakeServiceListResponse() const;
    [[nodiscard]] HttpResponse MakeStatusResponse(ProcessSupervisor& supervisor);
    [[nodiscard]] HttpResponse MakeActionResponse(ProcessSupervisor& supervisor, std::string_view action);
    [[nodiscard]] HttpResponse MakeLogsResponse(ProcessSupervisor& supervisor, std::size_t tail);
    [[nodiscard]] HttpResponse MakeMetricsResponse(ProcessSupervisor& supervisor);
    [[nodiscard]] HttpResponse MakeMetricsHistoryResponse(ProcessSupervisor& supervisor, std::size_t limit);
    [[nodiscard]] HttpResponse MakeMethodNotAllowed(std::string_view allow);

private:
    ServiceRegistry& registry_;
    MetricsCollector& metrics_collector_;
};
} // namespace aegis::agent
