//
// Created by Var on 2026/7/3.
//

#include "agent/agent_api.h"
#include "agent/log_reader.h"
#include "agent/metrics_collector.h"
#include "agent/process_supervisor.h"
#include "agent/service_metrics.h"
#include "agent/service_registry.h"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace aegis::agent {

namespace {
namespace http = boost::beast::http;
constexpr std::string_view kServicesPath{"/api/v1/services"};
constexpr std::string_view kServicePathPrefix{"/api/v1/services/"};
constexpr std::size_t kDefaultTailLimit = 100;
constexpr std::size_t kMaxTailLimit = 500;

constexpr std::size_t kDefaultHistoryLimit = 300;
constexpr std::size_t kMaxHistoryLimit = 300;

struct ServiceRoute {
    std::string_view service_id;
    std::string_view action;
};

[[nodiscard]] std::string_view RequestTarget(const HttpRequest& request) noexcept {
    const auto target = request.target();

    return {
        target.data(),
        target.size(),
    };
}

[[nodiscard]] std::string_view PathOnly(const std::string_view target) noexcept {
    const std::size_t query_position = target.find('?');

    if (query_position == std::string_view::npos) {
        return target;
    }

    return target.substr(0, query_position);
}

[[nodiscard]] std::optional<ServiceRoute> ParseServiceRoute(const std::string_view path) {
    if (!path.starts_with(kServicePathPrefix)) {
        return std::nullopt;
    }

    const std::string_view suffix = path.substr(kServicePathPrefix.size());

    const std::size_t slash_position = suffix.find('/');

    if (slash_position == std::string_view::npos || slash_position == 0 || slash_position + 1 >= suffix.size()) {
        return std::nullopt;
    }

    return ServiceRoute{
        .service_id = suffix.substr(0, slash_position),
        .action = suffix.substr(slash_position + 1),
    };
}

[[nodiscard]] std::size_t ParseBoundedPositiveQueryParameter(const std::string_view target,
                                                             const std::string_view expected_key,
                                                             const std::size_t default_value,
                                                             const std::size_t maximum_value) {
    const std::size_t query_position = target.find('?');

    if (query_position == std::string_view::npos) {
        return default_value;
    }

    std::string_view remaining = target.substr(query_position + 1);

    while (!remaining.empty()) {
        const std::size_t separator_position = remaining.find('&');

        const std::string_view pair = remaining.substr(0, separator_position);

        const std::size_t equals_position = pair.find('=');

        if (equals_position != std::string_view::npos) {
            const std::string_view key = pair.substr(0, equals_position);

            const std::string_view value = pair.substr(equals_position + 1);

            if (key == expected_key) {
                std::size_t parsed = 0;

                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);

                if (error != std::errc{} || end != value.data() + value.size()) {
                    return default_value;
                }

                return std::clamp(parsed, std::size_t{1}, maximum_value);
            }
        }

        if (separator_position == std::string_view::npos) {
            break;
        }

        remaining.remove_prefix(separator_position + 1);
    }

    return default_value;
}

[[nodiscard]] std::size_t ParseTailLimit(const std::string_view target) {
    return ParseBoundedPositiveQueryParameter(target, "tail", kDefaultTailLimit, kMaxTailLimit);
}

[[nodiscard]] std::size_t ParseHistoryLimit(const std::string_view target) {
    return ParseBoundedPositiveQueryParameter(target, "limit", kDefaultHistoryLimit, kMaxHistoryLimit);
}

void AppendStatusFields(std::ostringstream& body, const ServiceStatus& status) {
    body << "\"state\":\"" << ToString(status.state) << "\",\"pid\":" << status.pid
         << ",\"uptime_seconds\":" << status.uptime.count() << ",\"last_exit_code\":";

    if (status.exit_code.has_value()) {
        body << *status.exit_code;
    } else {
        body << "null";
    }
}

void AppendOptionalDouble(std::ostringstream& body, const std::optional<double>& value) {
    if (!value.has_value()) {
        body << "null";
        return;
    }

    body << std::setprecision(8) << *value;
}

void AppendOptionalUint64(std::ostringstream& body, const std::optional<std::uint64_t>& value) {
    if (!value.has_value()) {
        body << "null";
        return;
    }

    body << *value;
}
} // namespace

AgentApi::AgentApi(ServiceRegistry& registry, MetricsCollector& metrics_collector)
    : registry_(registry)
    , metrics_collector_(metrics_collector) {}
HttpResponse AgentApi::Handle(const HttpRequest& request) {
    const std::string_view target = RequestTarget(request);
    const std::string_view path = PathOnly(target);

    // GET /api/v1/services
    if (path == kServicesPath) {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }
        return MakeServiceListResponse();
    }

    // /api/v1/services/{service_id}/{action}
    const std::optional<ServiceRoute> route = ParseServiceRoute(path);

    if (!route.has_value()) {
        return MakeErrorResponse(http::status::not_found, "not_found", "route not found");
    }

    if (!IsValidServiceId(route->service_id)) {
        return MakeErrorResponse(http::status::bad_request, "invalid_service_id",
                                 "service_id may contain only letters, digits, '_' and '-'");
    }

    ProcessSupervisor* supervisor = registry_.Find(route->service_id);

    if (supervisor == nullptr) {
        return MakeErrorResponse(http::status::not_found, "service_not_found",
                                 "service does not exist: " + std::string(route->service_id));
    }

    if (route->action == "status") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }
        return MakeStatusResponse(*supervisor);
    }

    if (route->action == "logs") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }
        return MakeLogsResponse(*supervisor, ParseTailLimit(target));
    }

    if (route->action == "metrics/history") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }

        return MakeMetricsHistoryResponse(*supervisor, ParseHistoryLimit(target));
    }

    if (route->action == "metrics") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }
        return MakeMetricsResponse(*supervisor);
    }

    if (route->action == "start" || route->action == "stop" || route->action == "restart") {
        if (request.method() != http::verb::post) {
            return MakeMethodNotAllowed("POST");
        }
        return MakeActionResponse(*supervisor, route->action);
    }

    return MakeErrorResponse(http::status::not_found, "not_found", "service action not found");
}
HttpResponse AgentApi::MakeServiceListResponse() const {
    const std::vector<ServiceSummary> services = registry_.ListServices();

    std::ostringstream body;

    body << "{\"services\":[";

    for (std::size_t index = 0; index < services.size(); ++index) {
        if (index > 0) {
            body << ',';
        }

        const ServiceSummary& service = services[index];

        body << "{\"id\":\"" << JsonEscape(service.id) << "\",\"display_name\":\"" << JsonEscape(service.display_name)
             << "\",\"auto_start\":" << (service.auto_start ? "true" : "false") << ',';

        AppendStatusFields(body, service.status);

        body << '}';
    }

    body << "]}";

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeStatusResponse(ProcessSupervisor& supervisor) {
    const ServiceDefinition& definition = supervisor.Definition();
    const ServiceStatus status = supervisor.GetStatus();

    std::ostringstream body;

    body << "{\"id\":\"" << JsonEscape(definition.id) << "\",\"name\":\"" << JsonEscape(definition.id)
         << "\",\"display_name\":\"" << JsonEscape(definition.display_name)
         << "\",\"auto_start\":" << (definition.auto_start ? "true" : "false") << ',';

    AppendStatusFields(body, status);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeActionResponse(ProcessSupervisor& supervisor, const std::string_view action) {
    std::string error;
    bool success = false;

    if (action == "start") {
        success = supervisor.Start(error);
    } else if (action == "stop") {
        success = supervisor.Stop(error);
    } else if (action == "restart") {
        success = supervisor.Restart(error);
    } else {
        return MakeErrorResponse(http::status::internal_server_error, "unknown_action", "unsupported internal action");
    }

    if (!success) {
        return MakeErrorResponse(http::status::conflict, "service_action_failed", error);
    }

    return MakeStatusResponse(supervisor);
}
HttpResponse AgentApi::MakeLogsResponse(ProcessSupervisor& supervisor, const std::size_t tail) {
    const ServiceDefinition& definition = supervisor.Definition();

    std::string error;

    const std::vector<std::string> lines = ReadLastLines(definition.log_path, tail, error);

    if (!error.empty()) {
        return MakeErrorResponse(http::status::internal_server_error, "log_read_failed", error);
    }

    std::ostringstream body;

    body << "{\"id\":\"" << JsonEscape(definition.id) << "\",\"name\":\"" << JsonEscape(definition.id)
         << "\",\"lines\":[";

    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            body << ',';
        }
        body << '"' << JsonEscape(lines[index]) << '"';
    }

    body << "]}";

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeMetricsResponse(ProcessSupervisor& supervisor) {
    const ServiceDefinition& definition = supervisor.Definition();

    const std::optional<ServiceMetrics> metrics = metrics_collector_.GetLatest(definition.id);

    // Agent 刚启动且后台采集器尚未完成第一轮采样时，
    // 服务存在，但暂时没有指标快照。
    if (!metrics.has_value()) {
        return MakeErrorResponse(http::status::service_unavailable, "metrics_not_ready",
                                 "metrics collector has not produced a snapshot yet");
    }

    if (metrics->service_id != definition.id || !metrics->IsStructurallyValid()) {
        return MakeErrorResponse(http::status::internal_server_error, "invalid_metrics_cache",
                                 "metrics collector returned an invalid cached snapshot");
    }

    std::ostringstream body;

    body << "{\"service_id\":\"" << JsonEscape(metrics->service_id)
         << "\",\"available\":" << (metrics->available ? "true" : "false") << ",\"pid\":" << metrics->pid
         << ",\"collected_at_unix_ms\":" << metrics->collected_at_unix_ms << ",\"cpu_percent\":";

    AppendOptionalDouble(body, metrics->cpu_percent);

    body << ",\"rss_bytes\":";

    AppendOptionalUint64(body, metrics->rss_bytes);

    body << ",\"thread_count\":";

    AppendOptionalUint64(body, metrics->thread_count);

    body << ",\"fd_count\":";

    AppendOptionalUint64(body, metrics->fd_count);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeMetricsHistoryResponse(ProcessSupervisor& supervisor, const std::size_t limit) {
    const ServiceDefinition& definition = supervisor.Definition();

    const std::optional<std::vector<ServiceMetrics>> history = metrics_collector_.GetHistory(definition.id, limit);

    if (!history.has_value()) {
        return MakeErrorResponse(http::status::service_unavailable, "metrics_not_ready",
                                 "metrics collector has not produced a history snapshot yet");
    }

    std::ostringstream body;

    body << "{\"service_id\":\"" << JsonEscape(definition.id) << "\",\"points\":[";

    for (std::size_t index = 0; index < history->size(); ++index) {
        const ServiceMetrics& metrics = (*history)[index];

        if (metrics.service_id != definition.id || !metrics.IsStructurallyValid()) {
            return MakeErrorResponse(http::status::internal_server_error, "invalid_metrics_cache",
                                     "metrics collector returned an invalid history point");
        }

        if (index > 0) {
            body << ',';
        }

        body << "{\"available\":" << (metrics.available ? "true" : "false") << ",\"pid\":" << metrics.pid
             << ",\"collected_at_unix_ms\":" << metrics.collected_at_unix_ms << ",\"cpu_percent\":";

        AppendOptionalDouble(body, metrics.cpu_percent);

        body << ",\"rss_bytes\":";

        AppendOptionalUint64(body, metrics.rss_bytes);

        body << ",\"thread_count\":";

        AppendOptionalUint64(body, metrics.thread_count);

        body << ",\"fd_count\":";

        AppendOptionalUint64(body, metrics.fd_count);

        body << '}';
    }

    body << "]}";

    return MakeJsonResponse(http::status::ok, body.str());
}

HttpResponse AgentApi::MakeMethodNotAllowed(const std::string_view allow) {
    HttpResponse response = MakeErrorResponse(http::status::method_not_allowed, "method_not_allowed",
                                              "request method is not allowed for this route");

    response.set(http::field::allow, allow);

    return response;
}
} // namespace aegis::agent