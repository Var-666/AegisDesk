//
// Created by Var on 2026/7/3.
//

#include "agent/api/agent_api.h"
#include "agent/alerts/alert_event.h"
#include "agent/health/health_monitor.h"
#include "agent/health/health_status.h"
#include "agent/metrics/metrics_collector.h"
#include "agent/metrics/service_metrics.h"
#include "agent/recovery/recovery_policy.h"
#include "agent/service/log_reader.h"
#include "agent/service/process_supervisor.h"
#include "agent/service/service_registry.h"

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

constexpr std::size_t kDefaultAlertResolvedLimit = 100;
constexpr std::size_t kMaxAlertResolvedLimit = 500;

constexpr std::size_t kDefaultRecoveryEventLimit = 100;
constexpr std::size_t kMaxRecoveryEventLimit = 500;

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
    body << R"("state":")" << ToString(status.state) << R"(","desired_state":")" << ToString(status.desired_state)
         << R"(","pid":)" << status.pid << ",\"uptime_seconds\":" << status.uptime.count() << ",\"last_exit_code\":";

    if (status.exit_code.has_value()) {
        body << *status.exit_code;
    } else {
        body << "null";
    }

    body << ",\"process_group_id\":" << status.process_group_id << R"(,"last_exit_kind":")"
         << ToString(status.last_exit_kind) << R"(","last_exit_signal":)";

    if (status.last_exit_signal.has_value()) {
        body << *status.last_exit_signal;
    } else {
        body << "null";
    }

    body << R"(,"last_error":")" << JsonEscape(status.last_error) << R"(","last_transition_at_unix_ms":)"
         << status.last_transition_at_unix_ms;
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

void AppendOptionalString(std::ostringstream& body, const std::optional<std::string>& value) {
    if (!value.has_value()) {
        body << "null";
        return;
    }

    body << '"' << JsonEscape(*value) << '"';
}

void AppendHealthStatusJson(std::ostringstream& body, const HealthStatus& status) {
    body << R"({"service_id":")" << JsonEscape(status.service_id) << R"(","state":")" << ToString(status.state)
         << R"(","reason":")" << JsonEscape(status.reason) << R"(","consecutive_failures":)"
         << status.consecutive_failures << ",\"checked_at_unix_ms\":" << status.checked_at_unix_ms << '}';
}

void AppendAlertEventJson(std::ostringstream& body, const AlertEvent& event) {
    body << R"({"id":")" << JsonEscape(event.id) << R"(","service_id":")" << JsonEscape(event.service_id)
         << R"(","rule_id":")" << JsonEscape(event.rule_id) << R"(","severity":")" << ToString(event.severity)
         << R"(","state":")" << ToString(event.state) << R"(","message":")" << JsonEscape(event.message)
         << R"(","first_triggered_at_unix_ms":)" << event.first_triggered_at_unix_ms
         << ",\"last_triggered_at_unix_ms\":" << event.last_triggered_at_unix_ms << ",\"resolved_at_unix_ms\":";

    if (event.resolved_at_unix_ms.has_value()) {
        body << *event.resolved_at_unix_ms;
    } else {
        body << "null";
    }

    body << ",\"trigger_count\":" << event.trigger_count
         << ",\"acknowledged\":" << (event.acknowledged ? "true" : "false") << '}';
}

void AppendRecoveryEventJson(std::ostringstream& body, const RecoveryEvent& event) {
    body << R"({"service_id":")" << JsonEscape(event.service_id) << R"(","type":")" << ToString(event.type)
         << R"(","occurred_at_unix_ms":)" << event.occurred_at_unix_ms << R"(,"reason":")" << JsonEscape(event.reason)
         << R"(","alert_event_id":)";

    AppendOptionalString(body, event.alert_event_id);

    body << ",\"restart_count_in_window\":" << event.restart_count_in_window << '}';
}

void AppendAlertArrayJson(std::ostringstream& body, const std::vector<AlertEvent>& alerts) {
    body << '[';

    for (std::size_t index = 0; index < alerts.size(); ++index) {
        if (index > 0) {
            body << ',';
        }

        AppendAlertEventJson(body, alerts[index]);
    }

    body << ']';
}

void AppendRecoveryEventArrayJson(std::ostringstream& body, const std::vector<RecoveryEvent>& events) {
    body << '[';

    for (std::size_t index = 0; index < events.size(); ++index) {
        if (index > 0) {
            body << ',';
        }

        AppendRecoveryEventJson(body, events[index]);
    }

    body << ']';
}

[[nodiscard]] bool ParseBooleanQueryParameter(const std::string_view target, const std::string_view expected_key,
                                              const bool default_value) {
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
                return value == "true" || value == "1" || value == "yes";
            }
        }

        if (separator_position == std::string_view::npos) {
            break;
        }

        remaining.remove_prefix(separator_position + 1);
    }

    return default_value;
}

[[nodiscard]] bool ParseIncludeResolved(const std::string_view target) {
    return ParseBooleanQueryParameter(target, "include_resolved", false);
}

[[nodiscard]] std::size_t ParseAlertResolvedLimit(const std::string_view target) {
    return ParseBoundedPositiveQueryParameter(target, "limit", kDefaultAlertResolvedLimit, kMaxAlertResolvedLimit);
}

[[nodiscard]] std::size_t ParseRecoveryEventLimit(const std::string_view target) {
    return ParseBoundedPositiveQueryParameter(target, "limit", kDefaultRecoveryEventLimit, kMaxRecoveryEventLimit);
}

} // namespace

AgentApi::AgentApi(ServiceRegistry& registry, MetricsCollector& metrics_collector, HealthMonitor& health_monitor)
    : registry_(registry)
    , metrics_collector_(metrics_collector)
    , health_monitor_(health_monitor) {}
HttpResponse AgentApi::Handle(const HttpRequest& request) const {
    const std::string_view target = RequestTarget(request);
    const std::string_view path = PathOnly(target);

    if (path == "/api/v1/alerts") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }

        return MakeAllAlertsResponse(ParseIncludeResolved(target), ParseAlertResolvedLimit(target));
    }

    if (path == "/api/v1/alerts/active") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }

        return MakeActiveAlertsResponse();
    }

    if (path.starts_with("/api/v1/alerts/") && path.ends_with("/ack")) {
        if (request.method() != http::verb::post) {
            return MakeMethodNotAllowed("POST");
        }

        constexpr std::string_view prefix{"/api/v1/alerts/"};
        constexpr std::string_view suffix{"/ack"};

        const std::string_view alert_id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());

        return MakeAcknowledgeAlertResponse(alert_id);
    }

    if (path == "/api/v1/recovery-events") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }

        return MakeAllRecoveryEventsResponse(ParseRecoveryEventLimit(target));
    }

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

    if (route->action == "health") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }

        return MakeHealthResponse(*supervisor);
    }

    if (route->action == "alerts") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }

        return MakeServiceAlertsResponse(*supervisor, ParseIncludeResolved(target));
    }

    if (route->action == "recovery-events") {
        if (request.method() != http::verb::get) {
            return MakeMethodNotAllowed("GET");
        }

        return MakeServiceRecoveryEventsResponse(*supervisor, ParseRecoveryEventLimit(target));
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

        const auto& [id, display_name, auto_start, status] = services[index];

        body << R"({"id":")" << JsonEscape(id) << R"(","display_name":")" << JsonEscape(display_name)
             << R"(","auto_start":)" << (auto_start ? "true" : "false") << ',';

        AppendStatusFields(body, status);

        body << '}';
    }

    body << "]}";

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeStatusResponse(const ProcessSupervisor& supervisor) const {
    const ServiceDefinition& definition = supervisor.Definition();
    const ServiceStatus status = supervisor.GetStatus();

    std::ostringstream body;

    body << R"({"id":")" << JsonEscape(definition.id) << R"(","name":")" << JsonEscape(definition.id)
         << R"(","display_name":")" << JsonEscape(definition.display_name) << R"(","auto_start":)"
         << (definition.auto_start ? "true" : "false") << ',';

    AppendStatusFields(body, status);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeActionResponse(ProcessSupervisor& supervisor, const std::string_view action) const {
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
HttpResponse AgentApi::MakeLogsResponse(const ProcessSupervisor& supervisor, const std::size_t tail) const {
    const ServiceDefinition& definition = supervisor.Definition();

    std::string error;

    const std::vector<std::string> lines = ReadLastLines(definition.log_path, tail, error);

    if (!error.empty()) {
        return MakeErrorResponse(http::status::internal_server_error, "log_read_failed", error);
    }

    std::ostringstream body;

    body << R"({"id":")" << JsonEscape(definition.id) << "\",\"name\":\"" << JsonEscape(definition.id)
         << R"(","lines":[)";

    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            body << ',';
        }
        body << '"' << JsonEscape(lines[index]) << '"';
    }

    body << "]}";

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeMetricsResponse(const ProcessSupervisor& supervisor) const {
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

    body << R"({"service_id":")" << JsonEscape(metrics->service_id) << R"(","available":)"
         << (metrics->available ? "true" : "false") << ",\"pid\":" << metrics->pid
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
HttpResponse AgentApi::MakeMetricsHistoryResponse(const ProcessSupervisor& supervisor, const std::size_t limit) const {
    const ServiceDefinition& definition = supervisor.Definition();

    const std::optional<std::vector<ServiceMetrics>> history = metrics_collector_.GetHistory(definition.id, limit);

    if (!history.has_value()) {
        return MakeErrorResponse(http::status::service_unavailable, "metrics_not_ready",
                                 "metrics collector has not produced a history snapshot yet");
    }

    std::ostringstream body;

    body << R"({"service_id":")" << JsonEscape(definition.id) << "\",\"points\":[";

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
HttpResponse AgentApi::MakeHealthResponse(const ProcessSupervisor& supervisor) const {
    const ServiceDefinition& definition = supervisor.Definition();

    const std::optional<HealthStatus> health = health_monitor_.GetLatestHealth(definition.id);

    if (!health.has_value()) {
        return MakeErrorResponse(http::status::service_unavailable, "health_not_ready",
                                 "health monitor has not produced a status snapshot yet");
    }

    if (!health->IsStructurallyValid() || health->service_id != definition.id) {
        return MakeErrorResponse(http::status::internal_server_error, "invalid_health_cache",
                                 "health monitor returned an invalid cached status");
    }

    std::ostringstream body;

    AppendHealthStatusJson(body, *health);

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeServiceAlertsResponse(const ProcessSupervisor& supervisor,
                                                 const bool include_resolved) const {
    const ServiceDefinition& definition = supervisor.Definition();

    const std::vector<AlertEvent> alerts = health_monitor_.GetAlertsForService(definition.id, include_resolved);

    std::ostringstream body;

    body << R"({"service_id":")" << JsonEscape(definition.id) << R"(","include_resolved":)"
         << (include_resolved ? "true" : "false") << ",\"alerts\":";

    AppendAlertArrayJson(body, alerts);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeAllAlertsResponse(const bool include_resolved, const std::size_t resolved_limit) const {
    std::vector<AlertEvent> alerts = health_monitor_.GetActiveAlerts();

    if (include_resolved) {
        std::vector<AlertEvent> resolved = health_monitor_.GetRecentResolvedAlerts(resolved_limit);

        alerts.insert(alerts.end(), resolved.begin(), resolved.end());
    }

    std::ostringstream body;

    body << "{\"include_resolved\":" << (include_resolved ? "true" : "false") << ",\"alerts\":";

    AppendAlertArrayJson(body, alerts);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeActiveAlertsResponse() const {
    const std::vector<AlertEvent> alerts = health_monitor_.GetActiveAlerts();

    std::ostringstream body;

    body << "{\"alerts\":";

    AppendAlertArrayJson(body, alerts);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}

HttpResponse AgentApi::MakeAcknowledgeAlertResponse(const std::string_view alert_id) const {
    if (!IsValidAlertEventId(alert_id)) {
        return MakeErrorResponse(http::status::bad_request, "invalid_alert_id", "alert_id has an invalid format");
    }

    const std::optional<AlertEvent> alert = health_monitor_.AcknowledgeAlertAndGet(alert_id);

    if (!alert.has_value()) {
        return MakeErrorResponse(http::status::not_found, "alert_not_found",
                                 "alert does not exist: " + std::string(alert_id));
    }

    std::ostringstream body;

    body << R"({"acknowledged":true,"alert":)";

    AppendAlertEventJson(body, *alert);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}

HttpResponse AgentApi::MakeServiceRecoveryEventsResponse(const ProcessSupervisor& supervisor,
                                                         const std::size_t limit) const {
    const ServiceDefinition& definition = supervisor.Definition();

    const std::vector<RecoveryEvent> events = health_monitor_.GetRecoveryEventsForService(definition.id, limit);

    std::ostringstream body;

    body << R"({"service_id":")" << JsonEscape(definition.id) << "\",\"events\":";

    AppendRecoveryEventArrayJson(body, events);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}
HttpResponse AgentApi::MakeAllRecoveryEventsResponse(const std::size_t limit) const {
    const std::vector<RecoveryEvent> events = health_monitor_.GetRecentRecoveryEvents(limit);

    std::ostringstream body;

    body << "{\"events\":";

    AppendRecoveryEventArrayJson(body, events);

    body << '}';

    return MakeJsonResponse(http::status::ok, body.str());
}

HttpResponse AgentApi::MakeMethodNotAllowed(const std::string_view allow) {
    HttpResponse response = MakeErrorResponse(http::status::method_not_allowed, "method_not_allowed",
                                              "request method is not allowed for this route");

    response.set(http::field::allow, allow);

    return response;
}
} // namespace aegis::agent
