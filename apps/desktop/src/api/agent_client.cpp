#include "desktop/api/agent_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QUrlQuery>

#include <cmath>
#include <limits>
#include <utility>

namespace aegis::desktop {
namespace {

constexpr char kServicesPath[] = "/api/v1/services";

AgentError MakeClientError(const QString& code, const QString& message) {
    AgentError error;
    error.code = code;
    error.message = message;
    return error;
}

bool ParseRequiredInt64(const QJsonValue& value, const qint64 minimum, qint64& result) {
    if (!value.isDouble()) {
        return false;
    }

    const double parsed = value.toDouble();

    if (!std::isfinite(parsed) || std::floor(parsed) != parsed || parsed < static_cast<double>(minimum)
        || parsed > static_cast<double>(std::numeric_limits<qint64>::max())) {
        return false;
    }

    result = static_cast<qint64>(parsed);
    return true;
}

bool ParseOptionalDouble(const QJsonValue& value, const double minimum, const double maximum,
                         std::optional<double>& result) {
    if (value.isNull()) {
        result.reset();
        return true;
    }

    if (!value.isDouble()) {
        return false;
    }

    const double parsed = value.toDouble();

    if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return false;
    }

    result = parsed;
    return true;
}

bool ParseOptionalUInt64(const QJsonValue& value, std::optional<quint64>& result) {
    if (value.isNull()) {
        result.reset();
        return true;
    }

    if (!value.isDouble()) {
        return false;
    }

    const double parsed = value.toDouble();

    if (!std::isfinite(parsed) || std::floor(parsed) != parsed || parsed < 0.0
        || parsed > static_cast<double>(std::numeric_limits<quint64>::max())) {
        return false;
    }

    result = static_cast<quint64>(parsed);
    return true;
}

bool ParseRequiredUInt32(const QJsonValue& value, quint32& result) {
    if (!value.isDouble()) {
        return false;
    }

    const double parsed = value.toDouble();

    if (!std::isfinite(parsed) || std::floor(parsed) != parsed || parsed < 0.0
        || parsed > static_cast<double>(std::numeric_limits<quint32>::max())) {
        return false;
    }

    result = static_cast<quint32>(parsed);
    return true;
}

bool ParseOptionalInt64(const QJsonValue& value, qint64 minimum, std::optional<qint64>& result) {
    if (value.isNull()) {
        result.reset();
        return true;
    }

    qint64 parsed = 0;

    if (!ParseRequiredInt64(value, minimum, parsed)) {
        return false;
    }

    result = parsed;
    return true;
}

bool ParseOptionalString(const QJsonValue& value, std::optional<QString>& result) {
    if (value.isNull()) {
        result.reset();
        return true;
    }

    if (!value.isString()) {
        return false;
    }

    result = value.toString();
    return true;
}

} // namespace

AgentClient::AgentClient(QUrl base_url, QObject* parent)
    : QObject(parent)
    , base_url_(std::move(base_url)) {}

void AgentClient::GetServices(ServiceListCallback callback) {
    SendJsonRequest(
        HttpMethod::kGet, BuildUrl(kServicesPath),
        [callback = std::move(callback)](const std::optional<QJsonObject>& object, AgentError error) mutable {
            if (!object.has_value()) {
                callback(std::nullopt, std::move(error));
                return;
            }

            const QJsonValue services_value = object->value("services");

            if (!services_value.isArray()) {
                callback(std::nullopt, MakeClientError("invalid_response",
                                                       "Agent services response does not contain a services array"));
                return;
            }

            QList<ServiceSnapshot> services;
            QSet<QString> service_ids;

            for (const auto& value : services_value.toArray()) {
                if (!value.isObject()) {
                    callback(std::nullopt,
                             MakeClientError("invalid_response", "Agent services response contains a non-object item"));
                    return;
                }

                const std::optional<ServiceSnapshot> snapshot = ParseServiceSnapshot(value.toObject());

                if (!snapshot.has_value()) {
                    callback(std::nullopt, MakeClientError("invalid_response",
                                                           "Agent services response contains an invalid service"));
                    return;
                }

                if (service_ids.contains(snapshot->id)) {
                    callback(std::nullopt, MakeClientError("invalid_response",
                                                           "Agent services response contains duplicate service ids"));
                    return;
                }

                service_ids.insert(snapshot->id);
                services.push_back(*snapshot);
            }

            callback(services, {});
        });
}

void AgentClient::GetStatus(const QString& service_id, StatusCallback callback) {
    RequestStatus(HttpMethod::kGet, service_id, "status", std::move(callback));
}

void AgentClient::StartService(const QString& service_id, StatusCallback callback) {
    RequestStatus(HttpMethod::kPost, service_id, "start", std::move(callback));
}

void AgentClient::StopService(const QString& service_id, StatusCallback callback) {
    RequestStatus(HttpMethod::kPost, service_id, "stop", std::move(callback));
}

void AgentClient::RestartService(const QString& service_id, StatusCallback callback) {
    RequestStatus(HttpMethod::kPost, service_id, "restart", std::move(callback));
}

void AgentClient::GetLogs(const QString& service_id, const int tail, LogsCallback callback) {
    QUrl url = BuildServiceUrl(service_id, "logs");

    QUrlQuery query;
    query.addQueryItem("tail", QString::number(tail));

    url.setQuery(query);

    SendJsonRequest(
        HttpMethod::kGet, url,
        [callback = std::move(callback)](const std::optional<QJsonObject>& object, AgentError error) mutable {
            if (!object.has_value()) {
                callback(std::nullopt, std::move(error));
                return;
            }

            const QJsonValue lines_value = object->value("lines");

            if (!lines_value.isArray()) {
                callback(std::nullopt,
                         MakeClientError("invalid_response", "Agent logs response does not contain a lines array"));
                return;
            }

            QStringList lines;

            for (const auto& value : lines_value.toArray()) {
                if (value.isString()) {
                    lines.push_back(value.toString());
                }
            }

            callback(lines, {});
        });
}

void AgentClient::GetMetrics(const QString& service_id, MetricsCallback callback) {
    SendJsonRequest(
        HttpMethod::kGet, BuildServiceUrl(service_id, "metrics"),
        [service_id, callback = std::move(callback)](const std::optional<QJsonObject>& object,
                                                     AgentError error) mutable {
            if (!object.has_value()) {
                callback(std::nullopt, std::move(error));
                return;
            }

            const std::optional<ServiceMetricsSnapshot> metrics = ParseServiceMetricsSnapshot(*object);

            if (!metrics.has_value()) {
                callback(std::nullopt,
                         MakeClientError("invalid_response", "Agent metrics response has an invalid structure"));
                return;
            }

            if (metrics->service_id != service_id) {
                callback(std::nullopt, MakeClientError("invalid_response",
                                                       "Agent metrics response service_id does not match the request"));
                return;
            }

            callback(metrics, {});
        });
}

void AgentClient::GetMetricsHistory(const QString& service_id, const int limit, MetricsHistoryCallback callback) {
    QUrl url = BuildServiceUrl(service_id, "metrics/history");

    QUrlQuery query;

    query.addQueryItem("limit", QString::number(limit));

    url.setQuery(query);

    SendJsonRequest(
        HttpMethod::kGet, url,
        [service_id, callback = std::move(callback)](const std::optional<QJsonObject>& object,
                                                     AgentError error) mutable {
            if (!object.has_value()) {
                callback(std::nullopt, std::move(error));
                return;
            }

            const QJsonValue response_service_id = object->value("service_id");

            const QJsonValue points_value = object->value("points");

            if (!response_service_id.isString() || response_service_id.toString() != service_id
                || !points_value.isArray()) {
                callback(std::nullopt, MakeClientError("invalid_response",
                                                       "Agent metrics history response has an invalid structure"));

                return;
            }

            QList<ServiceMetricsSnapshot> points;

            qint64 previous_timestamp = -1;

            for (const auto& value : points_value.toArray()) {
                if (!value.isObject()) {
                    callback(std::nullopt,
                             MakeClientError("invalid_response", "Agent metrics history contains a non-object point"));

                    return;
                }

                const std::optional<ServiceMetricsSnapshot> point =
                    ParseServiceMetricsValues(value.toObject(), service_id);

                if (!point.has_value() || point->collected_at_unix_ms < previous_timestamp) {
                    callback(std::nullopt,
                             MakeClientError("invalid_response", "Agent metrics history contains an invalid point"));

                    return;
                }

                previous_timestamp = point->collected_at_unix_ms;

                points.push_back(*point);
            }

            callback(points, {});
        });
}

void AgentClient::GetHealth(const QString& service_id, HealthCallback callback) {
    SendJsonRequest(HttpMethod::kGet, BuildServiceUrl(service_id, "health"),
                    [service_id, callback = std::move(callback)](const std::optional<QJsonObject>& object,
                                                                 AgentError error) mutable {
                        if (!object.has_value()) {
                            callback(std::nullopt, std::move(error));
                            return;
                        }

                        const std::optional<HealthSnapshot> health = ParseHealthSnapshot(*object);

                        if (!health.has_value() || health->service_id != service_id) {
                            callback(std::nullopt, MakeClientError("invalid_response",
                                                                   "Agent health response has an invalid structure"));

                            return;
                        }

                        callback(health, {});
                    });
}

void AgentClient::GetServiceAlerts(const QString& service_id, const bool include_resolved, AlertsCallback callback) {
    QUrl url = BuildServiceUrl(service_id, "alerts");

    QUrlQuery query;
    query.addQueryItem("include_resolved", include_resolved ? "true" : "false");

    url.setQuery(query);

    SendJsonRequest(
        HttpMethod::kGet, url,
        [service_id, callback = std::move(callback)](const std::optional<QJsonObject>& object,
                                                     AgentError error) mutable {
            if (!object.has_value()) {
                callback(std::nullopt, std::move(error));
                return;
            }

            if (object->value("service_id").toString() != service_id || !object->value("alerts").isArray()) {
                callback(std::nullopt,
                         MakeClientError("invalid_response", "Agent alerts response has an invalid structure"));

                return;
            }

            QList<AlertSnapshot> alerts;

            for (const auto& value : object->value("alerts").toArray()) {
                if (!value.isObject()) {
                    callback(std::nullopt,
                             MakeClientError("invalid_response", "Agent alerts response contains a non-object item"));

                    return;
                }

                const std::optional<AlertSnapshot> alert = ParseAlertSnapshot(value.toObject());

                if (!alert.has_value() || alert->service_id != service_id) {
                    callback(std::nullopt,
                             MakeClientError("invalid_response", "Agent alerts response contains an invalid alert"));

                    return;
                }

                alerts.push_back(*alert);
            }

            callback(alerts, {});
        });
}

void AgentClient::AcknowledgeAlert(const QString& alert_id, AcknowledgeAlertCallback callback) {
    SendJsonRequest(
        HttpMethod::kPost, BuildUrl("/api/v1/alerts/" + alert_id + "/ack"),
        [callback = std::move(callback)](const std::optional<QJsonObject>& object, AgentError error) mutable {
            if (!object.has_value()) {
                callback(std::nullopt, std::move(error));
                return;
            }

            const QJsonValue alert_value = object->value("alert");

            if (!alert_value.isObject()) {
                callback(std::nullopt, MakeClientError("invalid_response",
                                                       "Agent acknowledge response does not contain alert object"));

                return;
            }

            const std::optional<AlertSnapshot> alert = ParseAlertSnapshot(alert_value.toObject());

            if (!alert.has_value()) {
                callback(std::nullopt,
                         MakeClientError("invalid_response", "Agent acknowledge response contains an invalid alert"));

                return;
            }

            callback(alert, {});
        });
}

void AgentClient::GetServiceRecoveryEvents(const QString& service_id, const int limit,
                                           RecoveryEventsCallback callback) {
    QUrl url = BuildServiceUrl(service_id, "recovery-events");

    QUrlQuery query;
    query.addQueryItem("limit", QString::number(limit));

    url.setQuery(query);

    SendJsonRequest(
        HttpMethod::kGet, url,
        [service_id, callback = std::move(callback)](const std::optional<QJsonObject>& object,
                                                     AgentError error) mutable {
            if (!object.has_value()) {
                callback(std::nullopt, std::move(error));
                return;
            }

            if (object->value("service_id").toString() != service_id || !object->value("events").isArray()) {
                callback(std::nullopt, MakeClientError("invalid_response",
                                                       "Agent recovery-events response has an invalid structure"));

                return;
            }

            QList<RecoveryEventSnapshot> events;

            for (const auto& value : object->value("events").toArray()) {
                if (!value.isObject()) {
                    callback(std::nullopt,
                             MakeClientError("invalid_response",
                                             "Agent recovery-events response contains a non-object item"));

                    return;
                }

                const std::optional<RecoveryEventSnapshot> event = ParseRecoveryEventSnapshot(value.toObject());

                if (!event.has_value() || event->service_id != service_id) {
                    callback(std::nullopt, MakeClientError("invalid_response",
                                                           "Agent recovery-events response contains an invalid event"));

                    return;
                }

                events.push_back(*event);
            }

            callback(events, {});
        });
}

QUrl AgentClient::BaseUrl() const {
    return base_url_;
}

void AgentClient::RequestStatus(const HttpMethod method, const QString& service_id, const QString& action,
                                StatusCallback callback) {
    SendJsonRequest(
        method, BuildServiceUrl(service_id, action),
        [callback = std::move(callback)](const std::optional<QJsonObject>& object, AgentError error) mutable {
            if (!object.has_value()) {
                callback(std::nullopt, std::move(error));
                return;
            }

            const std::optional<ServiceSnapshot> snapshot = ParseServiceSnapshot(*object);

            if (!snapshot.has_value()) {
                callback(std::nullopt,
                         MakeClientError("invalid_response", "Agent status response has an invalid structure"));
                return;
            }

            callback(snapshot, {});
        });
}

void AgentClient::SendJsonRequest(const HttpMethod method, const QUrl& url, JsonCallback callback) {
    QNetworkRequest request(url);

    request.setRawHeader("Accept", "application/json");

    QNetworkReply* reply = nullptr;

    if (method == HttpMethod::kGet) {
        reply = network_manager_.get(request);
    } else {
        reply = network_manager_.post(request, QByteArray{});
    }

    connect(reply, &QNetworkReply::finished, reply, [reply, callback = std::move(callback)]() mutable {
        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        const QByteArray body = reply->readAll();

        QJsonParseError parse_error;

        const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);

        const bool has_json_object = parse_error.error == QJsonParseError::NoError && document.isObject();

        const QJsonObject object = has_json_object ? document.object() : QJsonObject{};

        if (reply->error() != QNetworkReply::NoError) {
            callback(std::nullopt, ParseError(http_status, object, reply->errorString()));

            reply->deleteLater();
            return;
        }

        if (http_status < 200 || http_status >= 300) {
            callback(std::nullopt, ParseError(http_status, object, "Agent returned a non-success HTTP status"));

            reply->deleteLater();
            return;
        }

        if (!has_json_object) {
            callback(std::nullopt, MakeClientError("invalid_json", "Agent returned invalid JSON"));

            reply->deleteLater();
            return;
        }

        callback(object, {});
        reply->deleteLater();
    });
}

QUrl AgentClient::BuildUrl(const QString& path) const {
    QUrl url = base_url_;

    url.setPath(path);
    url.setQuery(QString{});

    return url;
}

QUrl AgentClient::BuildServiceUrl(const QString& service_id, const QString& action) const {
    return BuildUrl("/api/v1/services/" + service_id + "/" + action);
}

std::optional<ServiceSnapshot> AgentClient::ParseServiceSnapshot(const QJsonObject& object) {
    const QJsonValue id_value = object.value("id");
    const QJsonValue state_value = object.value("state");
    const QJsonValue desired_state_value = object.value("desired_state");
    const QJsonValue display_name_value = object.value("display_name");
    const QJsonValue auto_start_value = object.value("auto_start");
    const QJsonValue pid_value = object.value("pid");
    const QJsonValue uptime_value = object.value("uptime_seconds");
    const QJsonValue exit_code_value = object.value("last_exit_code");

    if (!id_value.isString() || id_value.toString().isEmpty() || !state_value.isString()
        || !desired_state_value.isString()
        || !display_name_value.isString() || !auto_start_value.isBool() || !pid_value.isDouble()
        || !uptime_value.isDouble()) {
        return std::nullopt;
    }

    if (!exit_code_value.isNull() && !exit_code_value.isDouble()) {
        return std::nullopt;
    }

    ServiceSnapshot snapshot;

    snapshot.id = id_value.toString();
    snapshot.display_name = display_name_value.toString();
    snapshot.state = state_value.toString();
    snapshot.desired_state = desired_state_value.toString();
    snapshot.auto_start = auto_start_value.toBool();
    snapshot.pid = static_cast<qint64>(pid_value.toDouble());
    snapshot.uptime_seconds = static_cast<qint64>(uptime_value.toDouble());

    if (exit_code_value.isDouble()) {
        snapshot.last_exit_code = static_cast<int>(exit_code_value.toDouble());
    }

    return snapshot;
}

std::optional<ServiceMetricsSnapshot> AgentClient::ParseServiceMetricsSnapshot(const QJsonObject& object) {
    const QJsonValue service_id_value = object.value("service_id");

    if (!service_id_value.isString() || service_id_value.toString().isEmpty()) {
        return std::nullopt;
    }

    return ParseServiceMetricsValues(object, service_id_value.toString());
}

AgentError AgentClient::ParseError(const int http_status, const QJsonObject& object, const QString& fallback_message) {
    AgentError error;

    error.http_status = http_status;
    error.code = object.value("error").toString();
    error.message = object.value("message").toString();

    if (error.code.isEmpty()) {
        error.code = http_status == 0 ? "network_error" : "http_error";
    }

    if (error.message.isEmpty()) {
        error.message = fallback_message;
    }

    return error;
}

std::optional<ServiceMetricsSnapshot> AgentClient::ParseServiceMetricsValues(const QJsonObject& object,
                                                                             const QString& service_id) {
    const QJsonValue available_value = object.value("available");

    if (service_id.isEmpty() || !available_value.isBool()) {
        return std::nullopt;
    }

    ServiceMetricsSnapshot snapshot;

    snapshot.service_id = service_id;
    snapshot.available = available_value.toBool();

    if (!ParseRequiredInt64(object.value("pid"), -1, snapshot.pid)
        || !ParseRequiredInt64(object.value("collected_at_unix_ms"), 0, snapshot.collected_at_unix_ms)
        || !ParseOptionalDouble(object.value("cpu_percent"), 0.0, 100.0, snapshot.cpu_percent)
        || !ParseOptionalUInt64(object.value("rss_bytes"), snapshot.rss_bytes)
        || !ParseOptionalUInt64(object.value("thread_count"), snapshot.thread_count)
        || !ParseOptionalUInt64(object.value("fd_count"), snapshot.fd_count)) {
        return std::nullopt;
    }

    if (!snapshot.available) {
        if (snapshot.cpu_percent.has_value() || snapshot.rss_bytes.has_value() || snapshot.thread_count.has_value()
            || snapshot.fd_count.has_value()) {
            return std::nullopt;
        }

        return snapshot;
    }

    if (snapshot.pid <= 0 || !snapshot.rss_bytes.has_value() || !snapshot.thread_count.has_value()
        || !snapshot.fd_count.has_value()) {
        return std::nullopt;
    }

    return snapshot;
}

std::optional<HealthSnapshot> AgentClient::ParseHealthSnapshot(const QJsonObject& object) {
    if (!object.value("service_id").isString() || !object.value("state").isString()
        || !object.value("reason").isString()) {
        return std::nullopt;
    }

    HealthSnapshot snapshot;

    snapshot.service_id = object.value("service_id").toString();

    snapshot.state = object.value("state").toString();

    snapshot.reason = object.value("reason").toString();

    if (!ParseRequiredUInt32(object.value("consecutive_failures"), snapshot.consecutive_failures)
        || !ParseRequiredInt64(object.value("checked_at_unix_ms"), 0, snapshot.checked_at_unix_ms)) {
        return std::nullopt;
    }

    return snapshot;
}

std::optional<AlertSnapshot> AgentClient::ParseAlertSnapshot(const QJsonObject& object) {
    if (!object.value("id").isString() || !object.value("service_id").isString() || !object.value("rule_id").isString()
        || !object.value("severity").isString() || !object.value("state").isString()
        || !object.value("message").isString() || !object.value("acknowledged").isBool()) {
        return std::nullopt;
    }

    AlertSnapshot snapshot;

    snapshot.id = object.value("id").toString();
    snapshot.service_id = object.value("service_id").toString();
    snapshot.rule_id = object.value("rule_id").toString();
    snapshot.severity = object.value("severity").toString();
    snapshot.state = object.value("state").toString();
    snapshot.message = object.value("message").toString();
    snapshot.acknowledged = object.value("acknowledged").toBool();

    if (!ParseRequiredInt64(object.value("first_triggered_at_unix_ms"), 0, snapshot.first_triggered_at_unix_ms)
        || !ParseRequiredInt64(object.value("last_triggered_at_unix_ms"), 0, snapshot.last_triggered_at_unix_ms)
        || !ParseOptionalInt64(object.value("resolved_at_unix_ms"), 0, snapshot.resolved_at_unix_ms)
        || !ParseRequiredUInt32(object.value("trigger_count"), snapshot.trigger_count)) {
        return std::nullopt;
    }

    return snapshot;
}

std::optional<RecoveryEventSnapshot> AgentClient::ParseRecoveryEventSnapshot(const QJsonObject& object) {
    if (!object.value("service_id").isString() || !object.value("type").isString()
        || !object.value("reason").isString()) {
        return std::nullopt;
    }

    RecoveryEventSnapshot snapshot;

    snapshot.service_id = object.value("service_id").toString();
    snapshot.type = object.value("type").toString();
    snapshot.reason = object.value("reason").toString();

    if (!ParseRequiredInt64(object.value("occurred_at_unix_ms"), 0, snapshot.occurred_at_unix_ms)
        || !ParseOptionalString(object.value("alert_event_id"), snapshot.alert_event_id)
        || !ParseRequiredUInt32(object.value("restart_count_in_window"), snapshot.restart_count_in_window)) {
        return std::nullopt;
    }

    return snapshot;
}

} // namespace aegis::desktop
