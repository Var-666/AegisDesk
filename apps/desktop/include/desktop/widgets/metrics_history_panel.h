#pragma once

#include "desktop/api/agent_client.h"

#include <QGroupBox>
#include <QString>
#include <QtGlobal>

class QLabel;

namespace aegis::desktop {

class MetricChartWidget;

class MetricsHistoryPanel final : public QGroupBox {
public:
    explicit MetricsHistoryPanel(AgentClient& agent_client, QWidget* parent = nullptr);

    void SetServiceId(const QString& service_id);

    void Refresh();

    void Clear();

private:
    void ApplyHistory(const QList<ServiceMetricsSnapshot>& history) const;

    void SetStatus(const QString& status) const;

    AgentClient& agent_client_;

    QString service_id_;

    bool request_in_flight_{false};
    quint64 request_generation_{0};

    QLabel* status_value_{nullptr};

    MetricChartWidget* cpu_chart_{nullptr};
    MetricChartWidget* rss_chart_{nullptr};
    MetricChartWidget* thread_chart_{nullptr};
    MetricChartWidget* fd_chart_{nullptr};
};

} // namespace aegis::desktop