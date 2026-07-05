#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <optional>

namespace aegis::desktop {
struct AgentError {
    int http_status{0};
    QString code;
    QString message;

    bool Ok() const {
        return message.isEmpty();
    }
};

struct ServiceSnapshot {
    QString name;
    QString state;

    qint64 pid{-1};
    qint64 uptime_seconds{0};

    std::optional<int> last_exit_code;
};

class AgentClient final : public QObject {
public:
    using StatusCallback = std::function<void(std::optional<ServiceSnapshot>, AgentError)>;
    using LogsCallback = std::function<void(std::optional<QStringList>, AgentError)>;

    explicit AgentClient(QUrl base_url, QObject* parent = nullptr);

    void GetStatus(StatusCallback callback);

    void StartService(StatusCallback callback);

    void StopService(StatusCallback callback);

    void RestartService(StatusCallback callback);

    void GetLogs(int tail, LogsCallback callback);

    QUrl BaseUrl() const;

private:
    enum class HttpMethod {
        kGet,
        kPost,
    };

    using JsonCallback = std::function<void(std::optional<QJsonObject>, AgentError)>;

    void RequestStatus(HttpMethod method, const QString& path, StatusCallback callback);

    void SendJsonRequest(HttpMethod method, const QUrl& url, JsonCallback callback);

    QUrl BuildUrl(const QString& path) const;

    static std::optional<ServiceSnapshot> ParseServiceSnapshot(const QJsonObject& object);

    static AgentError ParseError(int http_status, const QJsonObject& object, const QString& fallback_message);

private:
    QUrl base_url_;
    QNetworkAccessManager network_manager_;
};

} // namespace aegis::desktop
