#pragma once

#include "agent/http_json.h"

#include <filesystem>
#include <string_view>

namespace aegis::agent {

class ProcessSupervisor;
class ServiceRegistry;

class AgentApi {
public:
    explicit AgentApi(ServiceRegistry& registry);

    [[nodiscard]] HttpResponse Handle(const HttpRequest& request);

private:
    [[nodiscard]] HttpResponse MakeServiceListResponse() const;
    [[nodiscard]] HttpResponse MakeStatusResponse(ProcessSupervisor& supervisor);
    [[nodiscard]] HttpResponse MakeActionResponse(ProcessSupervisor& supervisor, std::string_view action);
    [[nodiscard]] HttpResponse MakeLogsResponse(ProcessSupervisor& supervisor, std::size_t tail);
    [[nodiscard]] HttpResponse MakeMethodNotAllowed(std::string_view allow);

private:
    ServiceRegistry& registry_;
};
} // namespace aegis::agent
