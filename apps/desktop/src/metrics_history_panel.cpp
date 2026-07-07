#include "desktop/metrics_history_panel.h"

#include "desktop/metric_chart_widget.h"

#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

namespace aegis::desktop {
namespace {

constexpr int kHistoryLimit = 300;

} // namespace

MetricsHistoryPanel::MetricsHistoryPanel(AgentClient& agent_client, QWidget* parent)
    : QGroupBox("Metrics History", parent)
    , agent_client_(agent_client) {
    auto* root_layout = new QVBoxLayout(this);

    auto* status_layout = new QHBoxLayout();

    status_layout->addWidget(new QLabel("History Status:", this));

    status_value_ = new QLabel("-", this);

    status_layout->addWidget(status_value_);
    status_layout->addStretch();

    root_layout->addLayout(status_layout);

    auto* charts_layout = new QGridLayout();

    cpu_chart_ = new MetricChartWidget(MetricChartKind::kCpuPercent, this);
    rss_chart_ = new MetricChartWidget(MetricChartKind::kRssMemory, this);
    thread_chart_ = new MetricChartWidget(MetricChartKind::kThreadCount, this);
    fd_chart_ = new MetricChartWidget(MetricChartKind::kFdCount, this);

    charts_layout->addWidget(cpu_chart_, 0, 0);
    charts_layout->addWidget(rss_chart_, 0, 1);
    charts_layout->addWidget(thread_chart_, 1, 0);
    charts_layout->addWidget(fd_chart_, 1, 1);

    root_layout->addLayout(charts_layout);

    Clear();
}

void MetricsHistoryPanel::SetServiceId(const QString& service_id) {
    if (service_id == service_id_) {
        return;
    }

    service_id_ = service_id;

    ++request_generation_;
    request_in_flight_ = false;

    cpu_chart_->Clear();
    rss_chart_->Clear();
    thread_chart_->Clear();
    fd_chart_->Clear();

    SetStatus(service_id_.isEmpty() ? "No service selected" : "Loading history...");

    Refresh();
}

void MetricsHistoryPanel::Refresh() {
    if (service_id_.isEmpty() || request_in_flight_) {
        return;
    }

    request_in_flight_ = true;

    const QString requested_service_id = service_id_;

    const quint64 generation = ++request_generation_;

    const QPointer<MetricsHistoryPanel> self(this);

    agent_client_.GetMetricsHistory(
        requested_service_id, kHistoryLimit,
        [self, requested_service_id, generation](std::optional<QList<ServiceMetricsSnapshot>> history,
                                                 AgentError error) {
            if (self.isNull() || generation != self->request_generation_ || requested_service_id != self->service_id_) {
                return;
            }

            self->request_in_flight_ = false;

            if (!history.has_value()) {
                if (error.code == "metrics_not_ready") {
                    self->SetStatus("Waiting for first metrics sample");
                    return;
                }

                self->SetStatus("History unavailable: " + error.message);
                return;
            }

            self->ApplyHistory(*history);
        });
}

void MetricsHistoryPanel::Clear() {
    service_id_.clear();

    ++request_generation_;
    request_in_flight_ = false;

    cpu_chart_->Clear();
    rss_chart_->Clear();
    thread_chart_->Clear();
    fd_chart_->Clear();

    SetStatus("No service selected");
}

void MetricsHistoryPanel::ApplyHistory(const QList<ServiceMetricsSnapshot>& history) {
    if (history.isEmpty()) {
        SetStatus("No history samples yet");

        cpu_chart_->Clear("No history samples");
        rss_chart_->Clear("No history samples");
        thread_chart_->Clear("No history samples");
        fd_chart_->Clear("No history samples");

        return;
    }

    cpu_chart_->SetHistory(history);
    rss_chart_->SetHistory(history);
    thread_chart_->SetHistory(history);
    fd_chart_->SetHistory(history);

    const ServiceMetricsSnapshot& last_point = history.back();

    const QString sample_time =
        QDateTime::fromMSecsSinceEpoch(last_point.collected_at_unix_ms).toLocalTime().toString("HH:mm:ss");

    SetStatus(QString("%1 samples, latest: %2").arg(history.size()).arg(sample_time));
}

void MetricsHistoryPanel::SetStatus(const QString& status) {
    status_value_->setText(status);
}

} // namespace aegis::desktop