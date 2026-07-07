#pragma once

#include "desktop/agent_client.h"

#include <QtCharts/QChartView>

#include <optional>

#include <QList>
#include <QString>

class QDateTimeAxis;
class QValueAxis;

namespace aegis::desktop {

enum class MetricChartKind {
    kCpuPercent,
    kRssMemory,
    kThreadCount,
    kFdCount,
};

class MetricChartWidget final : public QChartView {
public:
    explicit MetricChartWidget(MetricChartKind kind, QWidget* parent = nullptr);

    void SetHistory(const QList<ServiceMetricsSnapshot>& history);

    void Clear(const QString& message = "No history samples");

private:
    [[nodiscard]] static QString TitleFor(MetricChartKind kind);

    [[nodiscard]] static QString AxisTitleFor(MetricChartKind kind);

    [[nodiscard]] static std::optional<double> ValueFor(MetricChartKind kind, const ServiceMetricsSnapshot& point);

    void RemoveAllSeries();

    MetricChartKind kind_;

    QChart* chart_{nullptr};
    QDateTimeAxis* time_axis_{nullptr};
    QValueAxis* value_axis_{nullptr};
};

} // namespace aegis::desktop