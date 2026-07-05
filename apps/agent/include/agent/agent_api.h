#pragma once

#include "agent/http_json.h"

#include <filesystem>
#include <string_view>

namespace aegis::agent {

class ProcessSupervisor;

class AgentApi {
public:
    AgentApi(ProcessSupervisor& supervisor, std::filesystem::path log_path);

    HttpResponse Handle(const HttpRequest& request);

private:
    HttpResponse MakeStatusResponse() const;
    HttpResponse MakeActionResponse(std::string_view action) const;
    HttpResponse MakeLogsResponse(std::size_t tail) const;

private:
    ProcessSupervisor& supervisor_;
    std::filesystem::path log_path_;
};
} // namespace aegis::agent
