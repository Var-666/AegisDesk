#pragma once

#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtGlobal>

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

struct ServiceMetricsSnapshot {
    QString service_id;

    bool available{false};

    qint64 pid{-1};
    qint64 collected_at_unix_ms{0};

    std::optional<double> cpu_percent;
    std::optional<quint64> rss_bytes;
    std::optional<quint64> thread_count;
    std::optional<quint64> fd_count;
};

class AgentClient final : public QObject {
public:
    using StatusCallback = std::function<void(std::optional<ServiceSnapshot>, AgentError)>;

    using ServiceListCallback = std::function<void(std::optional<QList<ServiceSnapshot>>, AgentError)>;

    using LogsCallback = std::function<void(std::optional<QStringList>, AgentError)>;

    using MetricsCallback = std::function<void(std::optional<ServiceMetricsSnapshot>, AgentError)>;

    using MetricsHistoryCallback = std::function<void(std::optional<QList<ServiceMetricsSnapshot>>, AgentError)>;

    explicit AgentClient(QUrl base_url, QObject* parent = nullptr);

    void GetServices(ServiceListCallback callback);

    void GetStatus(const QString& service_id, StatusCallback callback);

    void StartService(const QString& service_id, StatusCallback callback);

    void StopService(const QString& service_id, StatusCallback callback);

    void RestartService(const QString& service_id, StatusCallback callback);

    void GetLogs(const QString& service_id, int tail, LogsCallback callback);

    void GetMetrics(const QString& service_id, MetricsCallback callback);

    void GetMetricsHistory(const QString& service_id, int limit, MetricsHistoryCallback callback);

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

    [[nodiscard]] static std::optional<ServiceMetricsSnapshot> ParseServiceMetricsSnapshot(const QJsonObject& object);

    [[nodiscard]] static AgentError ParseError(int http_status, const QJsonObject& object,
                                               const QString& fallback_message);

    [[nodiscard]] static std::optional<ServiceMetricsSnapshot> ParseServiceMetricsValues(const QJsonObject& object,
                                                                                         const QString& service_id);

    QUrl base_url_;
    QNetworkAccessManager network_manager_;
};

} // namespace aegis::desktop