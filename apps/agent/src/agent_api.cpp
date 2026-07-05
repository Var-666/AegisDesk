//
// Created by Var on 2026/7/3.
//

#include "agent/agent_api.h"
#include "agent/log_reader.h"
#include "agent/process_supervisor.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace aegis::agent {

namespace {
namespace http = boost::beast::http;
constexpr std::string_view kServiceName("demo_service");
constexpr std::size_t kDefaultTailLimit = 100;
constexpr std::size_t kMaxTailLimit = 500;

std::string PathOnly(const std::string_view target) {
    const auto query_position = target.find('?');
    return std::string(target.substr(0, query_position));
}

std::size_t ParseTailLimit(std::string_view target) {
    const auto query_position = target.find('?');

    if (query_position == std::string_view::npos) {
        return kDefaultTailLimit;
    }

    const std::string_view query = target.substr(query_position + 1);

    constexpr std::string_view key{"tail="};

    const auto key_position = query.find(key);

    if (key_position == std::string_view::npos) {
        return kDefaultTailLimit;
    }

    const std::string_view value = query.substr(key_position + key.size());

    const auto separator_position = value.find('&');

    const std::string_view token = value.substr(0, separator_position);

    std::size_t parsed = 0;

    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), parsed);

    if (error != std::errc{} || end != token.data() + token.size()) {
        return kDefaultTailLimit;
    }

    return std::clamp(parsed, std::size_t{1}, kMaxTailLimit);
}
} // namespace

AgentApi::AgentApi(ProcessSupervisor& supervisor, std::filesystem::path log_path)
    : supervisor_(supervisor)
    , log_path_(std::move(log_path)) {}

HttpResponse AgentApi::Handle(const HttpRequest& request) {
    const std::string path = PathOnly(request.target());

    if (path == "/api/v1/services/demo_service/status") {
        if (request.method() != http::verb::get) {
            return MakeErrorResponse(http::status::method_not_allowed, "method_not_allowed", "use GET");
        }
        return MakeStatusResponse();
    }

    if (path == "/api/v1/services/demo_service/logs") {
        if (request.method() != http::verb::get) {
            return MakeErrorResponse(http::status::method_not_allowed, "method_not_allowed", "use GET");
        }
        return MakeLogsResponse(ParseTailLimit(request.target()));
    }

    if (path == "/api/v1/services/demo_service/start") {
        if (request.method() != http::verb::post) {
            return MakeErrorResponse(http::status::method_not_allowed, "method_not_allowed", "use POST");
        }
        return MakeActionResponse("start");
    }

    if (path == "/api/v1/services/demo_service/stop") {
        if (request.method() != http::verb::post) {
            return MakeErrorResponse(http::status::method_not_allowed, "method_not_allowed", "use POST");
        }
        return MakeActionResponse("stop");
    }

    if (path == "/api/v1/services/demo_service/restart") {
        if (request.method() != http::verb::post) {
            return MakeErrorResponse(http::status::method_not_allowed, "method_not_allowed", "use POST");
        }
        return MakeActionResponse("restart");
    }

    return MakeErrorResponse(http::status::not_found, "not_found", "route not found");
}
HttpResponse AgentApi::MakeStatusResponse() const {
    const auto [state, pid, exit_code, uptime] = supervisor_.GetStatus();

    std::ostringstream body;

    body << "{\"name\":\"" << kServiceName << "\",\"state\":\"" << ToString(state) << "\",\"pid\":" << pid
         << ",\"uptime_seconds\":" << uptime.count() << ",\"last_exit_code\":";

    if (exit_code.has_value()) {
        body << *exit_code;
    } else {
        body << "null";
    }

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeActionResponse(const std::string_view action) const {
    std::string error;
    bool success = false;

    if (action == "start") {
        success = supervisor_.Start(error);
    } else if (action == "stop") {
        success = supervisor_.Stop(error);
    } else if (action == "restart") {
        success = supervisor_.Restart(error);
    } else {
        return MakeErrorResponse(http::status::internal_server_error, "unknown_action", "unsupported internal action");
    }

    if (!success) {
        return MakeErrorResponse(http::status::conflict, "service_action_failed", error);
    }

    return MakeStatusResponse();
}
HttpResponse AgentApi::MakeLogsResponse(const std::size_t tail) const {
    std::string error;

    const std::vector<std::string> lines = ReadLastLines(log_path_, tail, error);

    if (!error.empty()) {
        return MakeErrorResponse(http::status::internal_server_error, "log_read_failed", error);
    }

    std::ostringstream body;

    body << "{\"name\":\"" << kServiceName << "\",\"lines\":[";

    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            body << ',';
        }

        body << '"' << JsonEscape(lines[index]) << '"';
    }

    body << "]}";

    return MakeJsonResponse(http::status::ok, body.str());
}
} // namespace aegis::agent