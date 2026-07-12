#include "desktop/widgets/main_window.h"
#include "desktop/common/ui_helpers.h"
#include "desktop/widgets/metrics_history_panel.h"
#include "desktop/widgets/service_health_panel.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <initializer_list>
#include <utility>

namespace aegis::desktop {
namespace {

constexpr int kRefreshIntervalMs = 1000;

constexpr int kServiceIdRole = Qt::UserRole;

QString ServiceListText(const ServiceSnapshot& service) {
    return QString("%1\n%2  [%3]").arg(service.display_name, service.id, service.state);
}

void SetLabelsText(std::initializer_list<QLabel*> labels, const QString& text) {
    for (QLabel* label : labels) {
        label->setText(text);
    }
}

} // namespace

MainWindow::MainWindow(QUrl agent_url, QWidget* parent)
    : QMainWindow(parent)
    , agent_client_(std::move(agent_url)) {
    BuildUi();

    setWindowTitle("AegisDesk - Service Console");
    resize(1200, 820);

    statusBar()->showMessage("Agent: " + agent_client_.BaseUrl().toString());

    connect(&refresh_timer_, &QTimer::timeout, this, &MainWindow::RefreshAll);

    refresh_timer_.start(kRefreshIntervalMs);

    RefreshAll();
}

void MainWindow::BuildUi() {
    auto* central_widget = new QWidget(this);
    auto* root_layout = new QVBoxLayout(central_widget);

    auto* splitter = new QSplitter(Qt::Horizontal, central_widget);

    splitter->addWidget(BuildServiceListPanel(splitter));
    splitter->addWidget(BuildDetailPanel(splitter));
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    root_layout->addWidget(splitter);

    setCentralWidget(central_widget);

    ConnectUiSignals();

    ClearCurrentServiceDetails();
}

QWidget* MainWindow::BuildServiceListPanel(QWidget* parent) {
    auto* services_group = new QGroupBox("Services", parent);
    auto* services_layout = new QVBoxLayout(services_group);

    service_list_ = new QListWidget(services_group);
    service_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    service_list_->setMinimumWidth(260);
    services_layout->addWidget(service_list_);

    return services_group;
}

QWidget* MainWindow::BuildDetailPanel(QWidget* parent) {
    auto* detail_panel = new QWidget(parent);
    auto* detail_layout = new QVBoxLayout(detail_panel);
    auto* detail_tabs = new QTabWidget(detail_panel);

    detail_tabs->addTab(BuildOverviewTab(detail_tabs), "Overview");
    detail_tabs->addTab(BuildHealthTab(detail_tabs), "Health");
    detail_tabs->addTab(BuildTrendsTab(detail_tabs), "Trends");

    detail_layout->addWidget(detail_tabs, 1);

    return detail_panel;
}

QWidget* MainWindow::BuildOverviewTab(QWidget* parent) {
    auto* overview_tab = new QWidget(parent);
    auto* overview_layout = new QVBoxLayout(overview_tab);

    overview_layout->addWidget(BuildServiceDetailsGroup(overview_tab));
    overview_layout->addWidget(BuildMetricsGroup(overview_tab));
    overview_layout->addWidget(BuildLogsGroup(overview_tab), 1);

    return overview_tab;
}

QWidget* MainWindow::BuildHealthTab(QWidget* parent) {
    auto* health_tab = new QWidget(parent);
    auto* health_layout = new QVBoxLayout(health_tab);

    service_health_panel_ = new ServiceHealthPanel(agent_client_, health_tab);

    health_layout->addWidget(service_health_panel_);

    return health_tab;
}

QWidget* MainWindow::BuildTrendsTab(QWidget* parent) {
    auto* trends_tab = new QWidget(parent);
    auto* trends_layout = new QVBoxLayout(trends_tab);

    metrics_history_panel_ = new MetricsHistoryPanel(agent_client_, trends_tab);

    trends_layout->addWidget(metrics_history_panel_, 1);

    return trends_tab;
}

QGroupBox* MainWindow::BuildServiceDetailsGroup(QWidget* parent) {
    details_group_ = new QGroupBox("Service Details", parent);

    auto* service_layout = new QGridLayout(details_group_);

    ui::AddValueRow(service_layout, 0, 0, "Display Name:", service_name_value_, details_group_);
    ui::AddValueRow(service_layout, 0, 2, "Service ID:", service_id_value_, details_group_);
    ui::AddValueRow(service_layout, 1, 0, "State:", state_value_, details_group_);
    ui::AddValueRow(service_layout, 1, 2, "PID:", pid_value_, details_group_);
    ui::AddValueRow(service_layout, 2, 0, "Uptime:", uptime_value_, details_group_);
    ui::AddValueRow(service_layout, 2, 2, "Auto Start:", auto_start_value_, details_group_);
    ui::AddValueRow(service_layout, 3, 0, "Last Exit:", last_exit_value_, details_group_);

    // 控制按钮区域
    auto* actions_layout = new QHBoxLayout();

    start_button_ = new QPushButton("Start", details_group_);
    stop_button_ = new QPushButton("Stop", details_group_);
    restart_button_ = new QPushButton("Restart", details_group_);

    actions_layout->addWidget(start_button_);
    actions_layout->addWidget(stop_button_);
    actions_layout->addWidget(restart_button_);
    actions_layout->addStretch();

    service_layout->addLayout(actions_layout, 4, 0, 1, 4);

    return details_group_;
}

QGroupBox* MainWindow::BuildMetricsGroup(QWidget* parent) {
    auto* metrics_group = new QGroupBox("Runtime Metrics", parent);
    auto* metrics_layout = new QGridLayout(metrics_group);

    metrics_layout->addWidget(ui::CreateMetricCard("CPU Usage", cpu_value_, metrics_group), 0, 0);
    metrics_layout->addWidget(ui::CreateMetricCard("RSS Memory", rss_value_, metrics_group), 0, 1);
    metrics_layout->addWidget(ui::CreateMetricCard("Threads", thread_count_value_, metrics_group), 1, 0);
    metrics_layout->addWidget(ui::CreateMetricCard("Open FDs", fd_count_value_, metrics_group), 1, 1);

    metrics_layout->addWidget(new QLabel("Metrics Status:", metrics_group), 2, 0);
    metrics_status_value_ = ui::CreateValueLabel(metrics_group);
    metrics_layout->addWidget(metrics_status_value_, 2, 1);

    metrics_layout->addWidget(new QLabel("Last Sample:", metrics_group), 3, 0);
    last_sample_value_ = ui::CreateValueLabel(metrics_group);
    metrics_layout->addWidget(last_sample_value_, 3, 1);

    return metrics_group;
}

QGroupBox* MainWindow::BuildLogsGroup(QWidget* parent) {
    auto* logs_group = new QGroupBox("Recent Logs", parent);
    auto* logs_layout = new QVBoxLayout(logs_group);
    auto* logs_toolbar = new QHBoxLayout();

    logs_toolbar->addWidget(new QLabel("Tail Lines:", logs_group));

    tail_selector_ = new QSpinBox(logs_group);

    tail_selector_->setRange(10, 500);
    tail_selector_->setSingleStep(10);
    tail_selector_->setValue(100);

    logs_toolbar->addWidget(tail_selector_);

    refresh_logs_button_ = new QPushButton("Refresh Logs", logs_group);

    logs_toolbar->addWidget(refresh_logs_button_);
    logs_toolbar->addStretch();

    logs_layout->addLayout(logs_toolbar);

    log_view_ = new QPlainTextEdit(logs_group);

    log_view_->setReadOnly(true);

    log_view_->setLineWrapMode(QPlainTextEdit::NoWrap);

    logs_layout->addWidget(log_view_);

    return logs_group;
}

void MainWindow::ConnectUiSignals() {
    connect(service_list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current, QListWidgetItem*) {
        if (current == nullptr) {
            current_service_id_.clear();
            ClearCurrentServiceDetails();
            return;
        }

        const QString service_id = current->data(kServiceIdRole).toString();

        SelectService(service_id);
    });

    connect(start_button_, &QPushButton::clicked, this, [this] { ExecuteAction("start", &AgentClient::StartService); });

    connect(stop_button_, &QPushButton::clicked, this, [this] { ExecuteAction("stop", &AgentClient::StopService); });

    connect(restart_button_, &QPushButton::clicked, this,
            [this] { ExecuteAction("restart", &AgentClient::RestartService); });

    connect(refresh_logs_button_, &QPushButton::clicked, this, &MainWindow::RefreshLogs);
}

void MainWindow::RefreshAll() {
    RefreshServices();
    RefreshStatus();
    RefreshLogs();
    RefreshMetrics();

    if (metrics_history_panel_ != nullptr) {
        metrics_history_panel_->Refresh();
    }

    if (service_health_panel_ != nullptr) {
        service_health_panel_->Refresh();
    }
}

void MainWindow::RefreshServices() {
    if (services_request_in_flight_) {
        return;
    }

    services_request_in_flight_ = true;

    const quint64 request_generation = ++services_generation_;

    agent_client_.GetServices(
        [this, request_generation](std::optional<QList<ServiceSnapshot>> services, AgentError error) {
            if (request_generation != services_generation_) {
                return;
            }

            services_request_in_flight_ = false;

            if (!services.has_value()) {
                ShowBackgroundError(error);
                return;
            }

            ApplyServiceList(*services);
        });
}

void MainWindow::RefreshStatus() {
    if (current_service_id_.isEmpty() || action_in_flight_ || status_request_in_flight_) {
        return;
    }

    status_request_in_flight_ = true;

    const QString service_id = current_service_id_;

    const quint64 request_generation = ++status_generation_;

    agent_client_.GetStatus(
        service_id, [this, service_id, request_generation](std::optional<ServiceSnapshot> snapshot, AgentError error) {
            if (request_generation != status_generation_ || service_id != current_service_id_) {
                return;
            }

            status_request_in_flight_ = false;

            if (!snapshot.has_value()) {
                ShowBackgroundError(error);
                return;
            }

            ApplySnapshot(*snapshot);
        });
}

void MainWindow::RefreshLogs() {
    if (current_service_id_.isEmpty() || logs_request_in_flight_) {
        return;
    }

    logs_request_in_flight_ = true;

    const QString service_id = current_service_id_;

    const quint64 request_generation = ++logs_generation_;

    agent_client_.GetLogs(service_id, tail_selector_->value(),
                          [this, service_id, request_generation](std::optional<QStringList> lines, AgentError error) {
                              if (request_generation != logs_generation_ || service_id != current_service_id_) {
                                  return;
                              }

                              logs_request_in_flight_ = false;

                              if (!lines.has_value()) {
                                  ShowBackgroundError(error);
                                  return;
                              }

                              log_view_->setPlainText(lines->join('\n'));

                              QScrollBar* scrollbar = log_view_->verticalScrollBar();

                              scrollbar->setValue(scrollbar->maximum());
                          });
}

void MainWindow::RefreshMetrics() {
    if (current_service_id_.isEmpty() || metrics_request_in_flight_) {
        return;
    }

    metrics_request_in_flight_ = true;

    const QString service_id = current_service_id_;

    const quint64 request_generation = ++metrics_generation_;

    agent_client_.GetMetrics(service_id, [this, service_id, request_generation](
                                             std::optional<ServiceMetricsSnapshot> metrics, AgentError error) {
        if (request_generation != metrics_generation_ || service_id != current_service_id_) {
            return;
        }

        metrics_request_in_flight_ = false;

        if (!metrics.has_value()) {
            if (error.code == "metrics_not_ready") {
                ShowMetricsNotReady();

                statusBar()->showMessage("Metrics collector is waiting for its first sample");

                return;
            }

            ShowBackgroundError(error);
            return;
        }

        ApplyMetrics(*metrics);
    });
}

void MainWindow::ApplyServiceList(const QList<ServiceSnapshot>& services) {
    const QString preferred_service_id = current_service_id_;

    QListWidgetItem* selected_item = nullptr;
    std::optional<ServiceSnapshot> selected_service;

    {
        const QSignalBlocker blocker(service_list_);

        service_list_->clear();

        for (const ServiceSnapshot& service : services) {
            auto* item = new QListWidgetItem(ServiceListText(service), service_list_);

            item->setData(kServiceIdRole, service.id);

            item->setToolTip(service.display_name + "\nID: " + service.id + "\nState: " + service.state);

            if (service.id == preferred_service_id) {
                selected_item = item;
                selected_service = service;
            }
        }

        if (selected_item == nullptr && service_list_->count() > 0) {
            selected_item = service_list_->item(0);

            const QString selected_id = selected_item->data(kServiceIdRole).toString();

            for (const ServiceSnapshot& service : services) {
                if (service.id == selected_id) {
                    selected_service = service;
                    break;
                }
            }
        }

        service_list_->setCurrentItem(selected_item);
    }

    if (selected_item == nullptr) {
        current_service_id_.clear();
        ClearCurrentServiceDetails();

        statusBar()->showMessage("Agent returned no service definitions");

        return;
    }

    const QString selected_id = selected_item->data(kServiceIdRole).toString();

    if (selected_id != current_service_id_) {
        SelectService(selected_id);
        return;
    }

    if (selected_service.has_value()) {
        ApplySnapshot(*selected_service);
    }
}

void MainWindow::SelectService(const QString& service_id) {
    if (service_id.isEmpty() || service_id == current_service_id_) {
        return;
    }

    current_service_id_ = service_id;

    ++status_generation_;
    ++logs_generation_;
    ++metrics_generation_;

    status_request_in_flight_ = false;
    logs_request_in_flight_ = false;
    metrics_request_in_flight_ = false;

    ClearCurrentServiceDetails();

    if (metrics_history_panel_ != nullptr) {
        metrics_history_panel_->SetServiceId(current_service_id_);
    }

    if (service_health_panel_ != nullptr) {
        service_health_panel_->SetServiceId(current_service_id_);
    }

    statusBar()->showMessage("Selected service: " + current_service_id_);

    RefreshStatus();
    RefreshLogs();
    RefreshMetrics();
}

void MainWindow::ExecuteAction(const QString& action_name, const StatusOperation operation) {
    if (action_in_flight_ || current_service_id_.isEmpty()) {
        return;
    }

    const QString service_id = current_service_id_;

    action_in_flight_ = true;

    ++status_generation_;
    ++logs_generation_;
    ++metrics_generation_;

    status_request_in_flight_ = false;
    logs_request_in_flight_ = false;
    metrics_request_in_flight_ = false;

    UpdateActionButtons();

    statusBar()->showMessage("Sending " + action_name + " request for " + service_id + "...");

    (agent_client_.*operation)(
        service_id, [this, service_id, action_name](std::optional<ServiceSnapshot> snapshot, AgentError error) {
            action_in_flight_ = false;

            if (!snapshot.has_value()) {
                UpdateActionButtons();

                QMessageBox::warning(this, "Agent Request Failed",
                                     action_name + " failed for " + service_id + ".\n\n" + error.message);
                return;
            }

            if (service_id == current_service_id_) {
                ApplySnapshot(*snapshot);
                RefreshLogs();
                RefreshMetrics();

                if (metrics_history_panel_ != nullptr) {
                    metrics_history_panel_->Refresh();
                }

                if (service_health_panel_ != nullptr) {
                    service_health_panel_->Refresh();
                }
            }

            RefreshServices();

            statusBar()->showMessage(action_name + " completed for " + service_id);

            UpdateActionButtons();
        });
}

void MainWindow::ApplySnapshot(const ServiceSnapshot& snapshot) {
    has_snapshot_ = true;
    current_state_ = snapshot.state.trimmed().toLower();

    details_group_->setTitle("Service Details - " + snapshot.display_name);

    service_name_value_->setText(snapshot.display_name);
    service_id_value_->setText(snapshot.id);
    state_value_->setText(snapshot.state);
    pid_value_->setText(QString::number(snapshot.pid));

    uptime_value_->setText(FormatUptime(snapshot.uptime_seconds));

    auto_start_value_->setText(snapshot.auto_start ? "true" : "false");

    if (snapshot.last_exit_code.has_value()) {
        last_exit_value_->setText(QString::number(*snapshot.last_exit_code));
    } else {
        last_exit_value_->setText("-");
    }

    UpdateActionButtons();
}

void MainWindow::ApplyMetrics(const ServiceMetricsSnapshot& metrics) {
    last_sample_value_->setText(FormatSampleTime(metrics.collected_at_unix_ms));

    if (!metrics.available) {
        metrics_status_value_->setText(metrics.pid > 0 ? "Unavailable" : "Service stopped");

        cpu_value_->setText("-");
        rss_value_->setText("-");
        thread_count_value_->setText("-");
        fd_count_value_->setText("-");

        return;
    }

    metrics_status_value_->setText("Available");

    if (metrics.cpu_percent.has_value()) {
        cpu_value_->setText(QString("%1 %").arg(*metrics.cpu_percent, 0, 'f', 2));
    } else {
        cpu_value_->setText("Calculating...");
    }

    rss_value_->setText(metrics.rss_bytes.has_value() ? FormatBytes(*metrics.rss_bytes) : "-");

    thread_count_value_->setText(metrics.thread_count.has_value() ? QString::number(*metrics.thread_count) : "-");

    fd_count_value_->setText(metrics.fd_count.has_value() ? QString::number(*metrics.fd_count) : "-");
}

void MainWindow::ClearCurrentServiceDetails() {
    has_snapshot_ = false;
    current_state_ = "unknown";

    details_group_->setTitle("Service Details");

    SetLabelsText({service_name_value_, service_id_value_, state_value_, pid_value_, uptime_value_, auto_start_value_,
                   last_exit_value_},
                  "-");

    log_view_->clear();

    ClearMetrics();

    if (metrics_history_panel_ != nullptr) {
        metrics_history_panel_->Clear();
    }

    if (service_health_panel_ != nullptr) {
        service_health_panel_->Clear();
    }

    UpdateActionButtons();
}

void MainWindow::ClearMetrics() {
    SetLabelsText(
        {metrics_status_value_, cpu_value_, rss_value_, thread_count_value_, fd_count_value_, last_sample_value_}, "-");
}

void MainWindow::ShowMetricsNotReady() {
    metrics_status_value_->setText("Waiting for first sample");

    SetLabelsText({cpu_value_, rss_value_, thread_count_value_, fd_count_value_, last_sample_value_}, "-");
}

void MainWindow::UpdateActionButtons() {
    const bool service_running = current_state_ == "running";

    const bool can_control = has_snapshot_ && !current_service_id_.isEmpty() && !action_in_flight_;

    start_button_->setEnabled(can_control && !service_running);

    stop_button_->setEnabled(can_control && service_running);

    restart_button_->setEnabled(can_control && service_running);

    refresh_logs_button_->setEnabled(!current_service_id_.isEmpty() && !logs_request_in_flight_);

    tail_selector_->setEnabled(!current_service_id_.isEmpty());

    service_list_->setEnabled(!action_in_flight_);
}

void MainWindow::ShowBackgroundError(const AgentError& error) {
    QString message = error.message;

    if (message.isEmpty()) {
        message = "Unknown Agent communication error";
    }

    statusBar()->showMessage("Agent unavailable: " + message);
}

QString MainWindow::FormatUptime(const qint64 total_seconds) {
    const qint64 days = total_seconds / 86400;
    const qint64 hours = (total_seconds % 86400) / 3600;
    const qint64 minutes = (total_seconds % 3600) / 60;
    const qint64 seconds = total_seconds % 60;

    if (days > 0) {
        return QString("%1d %2h %3m %4s").arg(days).arg(hours).arg(minutes).arg(seconds);
    }

    return QString("%1h %2m %3s").arg(hours).arg(minutes).arg(seconds);
}

QString MainWindow::FormatBytes(const quint64 bytes) {
    constexpr std::array<const char*, 5> kUnits{
        "B", "KB", "MB", "GB", "TB",
    };

    double value = static_cast<double>(bytes);
    std::size_t unit_index = 0;

    while (value >= 1024.0 && unit_index + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit_index;
    }

    int precision = 0;

    if (value < 10.0 && unit_index > 0) {
        precision = 2;
    } else if (value < 100.0 && unit_index > 0) {
        precision = 1;
    }

    return QString("%1 %2").arg(value, 0, 'f', precision).arg(kUnits[unit_index]);
}

QString MainWindow::FormatSampleTime(const qint64 unix_time_milliseconds) {
    if (unix_time_milliseconds <= 0) {
        return "-";
    }

    const QDateTime utc_time = QDateTime::fromMSecsSinceEpoch(unix_time_milliseconds, QTimeZone::UTC);

    return utc_time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
}

} // namespace aegis::desktop
