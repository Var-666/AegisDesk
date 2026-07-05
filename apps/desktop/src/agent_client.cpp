//
// Created by Var on 2026/7/4.
//

#include "desktop/agent_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <utility>

namespace aegis::desktop {

namespace {
constexpr char kStatusPath[] = "/api/v1/services/demo_service/status";
constexpr char kStartPath[] = "/api/v1/services/demo_service/start";
constexpr char kStopPath[] = "/api/v1/services/demo_service/stop";
constexpr char kRestartPath[] = "/api/v1/services/demo_service/restart";
constexpr char kLogsPath[] = "/api/v1/services/demo_service/logs";

AgentError MakeClientError(const QString& code, const QString& message) {
    AgentError error;
    error.code = code;
    error.message = message;
    return error;
}
} // namespace

AgentClient::AgentClient(QUrl base_url, QObject* parent)
    : QObject(parent)
    , base_url_(base_url) {}

void AgentClient::GetStatus(StatusCallback callback) {
    RequestStatus(HttpMethod::kGet, kStatusPath, std::move(callback));
}

void AgentClient::StartService(StatusCallback callback) {
    RequestStatus(HttpMethod::kPost, kStartPath, std::move(callback));
}

void AgentClient::StopService(StatusCallback callback) {
    RequestStatus(HttpMethod::kPost, kStopPath, std::move(callback));
}

void AgentClient::RestartService(StatusCallback callback) {
    RequestStatus(HttpMethod::kPost, kRestartPath, std::move(callback));
}
void AgentClient::GetLogs(const int tail, LogsCallback callback) {
    QUrl url = BuildUrl(kLogsPath);

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

            for (const auto& value : lines_value.toArray()) {
                if (value.isString()) {
                    lines.push_back(QString(value.toString()));
                }
            }

            callback(lines, {});
        });
}
QUrl AgentClient::BaseUrl() const {
    return base_url_;
}
void AgentClient::RequestStatus(HttpMethod method, const QString& path, StatusCallback callback) {
    SendJsonRequest(method, BuildUrl(path),
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
void AgentClient::SendJsonRequest(HttpMethod method, const QUrl& url, JsonCallback callback) {
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
std::optional<ServiceSnapshot> AgentClient::ParseServiceSnapshot(const QJsonObject& object) {
    const QJsonValue name_value = object.value("name");
    const QJsonValue state_value = object.value("state");

    if (!name_value.isString() || !state_value.isString()) {
        return std::nullopt;
    }

    ServiceSnapshot snapshot;

    snapshot.name = name_value.toString();
    snapshot.state = state_value.toString();

    snapshot.pid = static_cast<qint64>(object.value("pid").toDouble(-1));

    snapshot.uptime_seconds = static_cast<qint64>(object.value("uptime_seconds").toDouble(0));

    const QJsonValue exit_code = object.value("last_exit_code");

    if (!exit_code.isNull() && exit_code.isDouble()) {
        snapshot.last_exit_code = static_cast<int>(exit_code.toDouble());
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
