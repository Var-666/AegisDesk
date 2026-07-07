#include "desktop/metric_chart_widget.h"

#include <QtCharts/QAbstractSeries>
#include <QtCharts/QChart>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <QDateTime>
#include <QPainter>
#include <QPointF>

#include <cmath>
#include <optional>

namespace aegis::desktop {
namespace {

constexpr qint64 kDefaultTimeWindowMs = 5 * 60 * 1000;

} // namespace

MetricChartWidget::MetricChartWidget(const MetricChartKind kind, QWidget* parent)
    : QChartView(parent)
    , kind_(kind)
    , chart_(new QChart()) {
    setChart(chart_);

    setRenderHint(QPainter::Antialiasing);

    setMinimumHeight(220);

    chart_->legend()->hide();

    // 创建横行时间轴
    time_axis_ = new QDateTimeAxis(chart_);
    time_axis_->setFormat("HH:mm:ss");
    time_axis_->setTickCount(5);

    // 创建竖向数值轴
    value_axis_ = new QValueAxis(chart_);
    value_axis_->setTitleText(AxisTitleFor(kind_));
    value_axis_->setTickCount(5);

    chart_->addAxis(time_axis_, Qt::AlignBottom);
    chart_->addAxis(value_axis_, Qt::AlignLeft);

    Clear();
}

void MetricChartWidget::SetHistory(const QList<ServiceMetricsSnapshot>& history) {
    RemoveAllSeries();

    qint64 min_time = -1;
    qint64 max_time = -1;

    double maximum_value = 0.0;

    QList<QPointF> segment_points;

    qint64 current_pid = -1;

    const auto flush_segment = [&] {
        if (segment_points.isEmpty()) {
            return;
        }

        auto* series = new QLineSeries();

        for (const QPointF& point : segment_points) {
            series->append(point);
        }

        chart_->addSeries(series);
        series->attachAxis(time_axis_);
        series->attachAxis(value_axis_);

        segment_points.clear();
        current_pid = -1;
    };

    for (const ServiceMetricsSnapshot& point : history) {
        if (point.collected_at_unix_ms <= 0) {
            flush_segment();
            continue;
        }

        const std::optional<double> value = ValueFor(kind_, point);

        if (!point.available || !value.has_value()) {
            flush_segment();
            continue;
        }

        if (current_pid > 0 && point.pid != current_pid) {
            flush_segment();
        }

        current_pid = point.pid;

        segment_points.push_back(QPointF(static_cast<double>(point.collected_at_unix_ms), *value));

        maximum_value = std::max(maximum_value, *value);

        if (min_time < 0) {
            min_time = point.collected_at_unix_ms;
        }

        max_time = point.collected_at_unix_ms;
    }

    flush_segment();

    if (min_time < 0 || max_time < 0) {
        Clear("No available metric samples");
        return;
    }

    if (max_time - min_time < 10 * 1000) {
        min_time -= 5 * 1000;
        max_time += 5 * 1000;
    }

    time_axis_->setRange(QDateTime::fromMSecsSinceEpoch(min_time), QDateTime::fromMSecsSinceEpoch(max_time));

    double axis_maximum = 1.0;

    if (kind_ == MetricChartKind::kCpuPercent) {
        axis_maximum = std::max(1.0, std::ceil(maximum_value * 1.2 * 100.0) / 100.0);

        value_axis_->setLabelFormat("%.2f");
    } else {
        axis_maximum = std::max(1.0, std::ceil(maximum_value * 1.2));

        value_axis_->setLabelFormat(kind_ == MetricChartKind::kRssMemory ? "%.1f" : "%.0f");
    }

    value_axis_->setRange(0.0, axis_maximum);

    chart_->setTitle(TitleFor(kind_) + " (Recent History)");
}

void MetricChartWidget::Clear(const QString& message) {
    RemoveAllSeries();

    const QDateTime now = QDateTime::currentDateTime();

    time_axis_->setRange(now.addMSecs(-kDefaultTimeWindowMs), now);

    value_axis_->setRange(0.0, 1.0);

    chart_->setTitle(TitleFor(kind_) + " - " + message);
}

QString MetricChartWidget::TitleFor(const MetricChartKind kind) {
    switch (kind) {
        case MetricChartKind::kCpuPercent:
            return "CPU Usage";
        case MetricChartKind::kRssMemory:
            return "RSS Memory";
        case MetricChartKind::kThreadCount:
            return "Threads";
        case MetricChartKind::kFdCount:
            return "Open FDs";
    }

    return "Metric";
}

QString MetricChartWidget::AxisTitleFor(const MetricChartKind kind) {
    switch (kind) {
        case MetricChartKind::kCpuPercent:
            return "Percent";
        case MetricChartKind::kRssMemory:
            return "MiB";
        case MetricChartKind::kThreadCount:
            return "Count";
        case MetricChartKind::kFdCount:
            return "Count";
    }

    return "Value";
}

std::optional<double> MetricChartWidget::ValueFor(const MetricChartKind kind, const ServiceMetricsSnapshot& point) {
    switch (kind) {
        case MetricChartKind::kCpuPercent:
            return point.cpu_percent;

        case MetricChartKind::kRssMemory:
            if (!point.rss_bytes.has_value()) {
                return std::nullopt;
            }

            return static_cast<double>(*point.rss_bytes) / (1024.0 * 1024.0);

        case MetricChartKind::kThreadCount:
            if (!point.thread_count.has_value()) {
                return std::nullopt;
            }

            return static_cast<double>(*point.thread_count);

        case MetricChartKind::kFdCount:
            if (!point.fd_count.has_value()) {
                return std::nullopt;
            }

            return static_cast<double>(*point.fd_count);
    }

    return std::nullopt;
}

void MetricChartWidget::RemoveAllSeries() {
    const QList<QAbstractSeries*> series = chart_->series();

    for (QAbstractSeries* item : series) {
        chart_->removeSeries(item);
        delete item;
    }
}

} // namespace aegis::desktop