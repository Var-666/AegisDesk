#pragma once

#include <QJsonObject>
#include <QList>
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

    [[nodiscard]] bool Ok() const {
        return message.isEmpty();
    }
};

struct ServiceSnapshot {
    QString id;
    QString display_name;
    QString state;

    bool auto_start{false};

    qint64 pid{-1};
    qint64 uptime_seconds{0};

    std::optional<int> last_exit_code;
};

class AgentClient final : public QObject {
public:
    using StatusCallback = std::function<void(std::optional<ServiceSnapshot>, AgentError)>;

    using ServiceListCallback = std::function<void(std::optional<QList<ServiceSnapshot>>, AgentError)>;

    using LogsCallback = std::function<void(std::optional<QStringList>, AgentError)>;

    explicit AgentClient(QUrl base_url, QObject* parent = nullptr);

    void GetServices(ServiceListCallback callback);

    void GetStatus(const QString& service_id, StatusCallback callback);

    void StartService(const QString& service_id, StatusCallback callback);

    void StopService(const QString& service_id, StatusCallback callback);

    void RestartService(const QString& service_id, StatusCallback callback);

    void GetLogs(const QString& service_id, int tail, LogsCallback callback);

    [[nodiscard]] QUrl BaseUrl() const;

private:
    enum class HttpMethod {
        kGet,
        kPost,
    };

    using JsonCallback = std::function<void(std::optional<QJsonObject>, AgentError)>;

    void RequestStatus(HttpMethod method, const QString& service_id, const QString& action, StatusCallback callback);

    void SendJsonRequest(HttpMethod method, const QUrl& url, JsonCallback callback);

    [[nodiscard]] QUrl BuildUrl(const QString& path) const;

    [[nodiscard]] QUrl BuildServiceUrl(const QString& service_id, const QString& action) const;

    [[nodiscard]] static std::optional<ServiceSnapshot> ParseServiceSnapshot(const QJsonObject& object);

    [[nodiscard]] static AgentError ParseError(int http_status, const QJsonObject& object,
                                               const QString& fallback_message);

    QUrl base_url_;
    QNetworkAccessManager network_manager_;
};

} // namespace aegis::desktop