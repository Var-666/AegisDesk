#include "desktop/agent_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QUrlQuery>

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

} // namespace

AgentClient::AgentClient(QUrl base_url, QObject* parent)
    : QObject(parent)
    , base_url_(std::move(base_url)) {}

void AgentClient::GetServices(ServiceListCallback callback) {
    SendJsonRequest(
        HttpMethod::kGet, BuildUrl(kServicesPath),
        [callback = std::move(callback)](std::optional<QJsonObject> object, AgentError error) mutable {
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

            for (const QJsonValue& value : services_value.toArray()) {
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
        [callback = std::move(callback)](std::optional<QJsonObject> object, AgentError error) mutable {
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

            for (const QJsonValue& value : lines_value.toArray()) {
                if (value.isString()) {
                    lines.push_back(value.toString());
                }
            }

            callback(lines, {});
        });
}

QUrl AgentClient::BaseUrl() const {
    return base_url_;
}

void AgentClient::RequestStatus(const HttpMethod method, const QString& service_id, const QString& action,
                                StatusCallback callback) {
    SendJsonRequest(method, BuildServiceUrl(service_id, action),
                    [callback = std::move(callback)](std::optional<QJsonObject> object, AgentError error) mutable {
                        if (!object.has_value()) {
                            callback(std::nullopt, std::move(error));
                            return;
                        }

                        const std::optional<ServiceSnapshot> snapshot = ParseServiceSnapshot(*object);

                        if (!snapshot.has_value()) {
                            callback(std::nullopt, MakeClientError("invalid_response",
                                                                   "Agent status response has an invalid structure"));
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
            callback(std::nullopt, AgentClient::ParseError(http_status, object, reply->errorString()));

            reply->deleteLater();
            return;
        }

        if (http_status < 200 || http_status >= 300) {
            callback(std::nullopt,
                     AgentClient::ParseError(http_status, object, "Agent returned a non-success HTTP status"));

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

    const QJsonValue display_name_value = object.value("display_name");

    const QJsonValue auto_start_value = object.value("auto_start");

    const QJsonValue pid_value = object.value("pid");

    const QJsonValue uptime_value = object.value("uptime_seconds");

    const QJsonValue exit_code_value = object.value("last_exit_code");

    if (!id_value.isString() || id_value.toString().isEmpty() || !state_value.isString()
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
    snapshot.auto_start = auto_start_value.toBool();

    snapshot.pid = static_cast<qint64>(pid_value.toDouble());

    snapshot.uptime_seconds = static_cast<qint64>(uptime_value.toDouble());

    if (exit_code_value.isDouble()) {
        snapshot.last_exit_code = static_cast<int>(exit_code_value.toDouble());
    }

    return snapshot;
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

} // namespace aegis::desktop