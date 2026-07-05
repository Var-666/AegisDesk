//
// Created by Var on 2026/7/4.
//

#include "desktop/main_window.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace aegis::desktop {
namespace {
constexpr int kRefreshIntervalMs = 100;

QLabel* CreateValueLabel(QWidget* parent) {
    auto* label = new QLabel("-", parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
} // namespace
MainWindow::MainWindow(QUrl agent_url, QWidget* parent)
    : QMainWindow(parent)
    , agent_client_(std::move(agent_url)) {
    BuildUI();

    setWindowTitle("AegisDesk - Service Console");
    resize(1000, 720);

    statusBar()->showMessage("Agent: " + agent_client_.BaseUrl().toString());

    // 定时刷新
    connect(&refresh_timer_, &QTimer::timeout, this, &MainWindow::RefreshAll);
    refresh_timer_.start(kRefreshIntervalMs);

    RefreshAll();
}
void MainWindow::BuildUI() {
    auto* central_widget = new QWidget(this);
    auto* root_layout = new QVBoxLayout(central_widget);

    auto* service_group = new QGroupBox("demo_service Runtime Status", central_widget);
    auto* service_layout = new QGridLayout(service_group);

    service_layout->addWidget(new QLabel("Service Name:", service_group), 0, 0);
    service_name_value_ = CreateValueLabel(service_group);
    service_layout->addWidget(service_name_value_, 0, 1);

    service_layout->addWidget(new QLabel("State:", service_group), 0, 2);
    state_value_ = CreateValueLabel(service_group);
    service_layout->addWidget(state_value_, 0, 3);

    service_layout->addWidget(new QLabel("PID:", service_group), 1, 0);
    pid_value_ = CreateValueLabel(service_group);
    service_layout->addWidget(pid_value_, 1, 1);

    service_layout->addWidget(new QLabel("Uptime:", service_group), 1, 2);
    uptime_value_ = CreateValueLabel(service_group);
    service_layout->addWidget(uptime_value_, 1, 3);

    service_layout->addWidget(new QLabel("Last Exit:", service_group), 2, 0);
    last_exit_value_ = CreateValueLabel(service_group);
    service_layout->addWidget(last_exit_value_, 2, 1);

    auto* actions_layout = new QHBoxLayout();

    start_button_ = new QPushButton("Start", service_group);
    stop_button_ = new QPushButton("Stop", service_group);
    restart_button_ = new QPushButton("Restart", service_group);

    actions_layout->addWidget(start_button_);
    actions_layout->addWidget(stop_button_);
    actions_layout->addWidget(restart_button_);
    actions_layout->addStretch();

    service_layout->addLayout(actions_layout, 3, 0, 1, 4);

    root_layout->addWidget(service_group);

    auto* logs_group = new QGroupBox("Recent Logs", central_widget);
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
    root_layout->addWidget(logs_group, 1);

    setCentralWidget(central_widget);

    connect(start_button_, &QPushButton::clicked, this, [this] { ExecuteAction("start", &AgentClient::StartService); });
    connect(stop_button_, &QPushButton::clicked, this, [this] { ExecuteAction("stop", &AgentClient::StopService); });
    connect(restart_button_, &QPushButton::clicked, this,
            [this] { ExecuteAction("restart", &AgentClient::RestartService); });
    connect(refresh_logs_button_, &QPushButton::clicked, this, &MainWindow::RefreshLogs);

    UpdateActionButtons();
}
void MainWindow::RefreshAll() {
    RefreshStatus();
    RefreshLogs();
}
void MainWindow::RefreshStatus() {
    if (action_in_flight_ || status_request_in_flight_) {
        return;
    }

    status_request_in_flight_ = true;

    const quint64 request_generation = ++status_generation_;

    agent_client_.GetStatus([this, request_generation](std::optional<ServiceSnapshot> snapshot, AgentError error) {
        if (request_generation != status_generation_) {
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
    if (logs_request_in_flight_) {
        return;
    }

    logs_request_in_flight_ = true;

    const quint64 request_generation = ++logs_generation_;

    agent_client_.GetLogs(tail_selector_->value(),
                          [this, request_generation](std::optional<QStringList> lines, AgentError error) {
                              if (request_generation != logs_generation_) {
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
void MainWindow::ExecuteAction(const QString& action_name, StatusOperation operation) {
    if (action_in_flight_) {
        return;
    }

    action_in_flight_ = true;

    ++status_generation_;
    status_request_in_flight_ = false;

    UpdateActionButtons();

    statusBar()->showMessage("Sending " + action_name + " request...");

    (agent_client_.*operation)([this, action_name](std::optional<ServiceSnapshot> snapshot, AgentError error) {
        action_in_flight_ = false;

        if (!snapshot.has_value()) {
            UpdateActionButtons();

            QMessageBox::warning(this, "Agent Request Failed", action_name + " failed.\n\n" + error.message);

            return;
        }

        ApplySnapshot(*snapshot);
        RefreshLogs();

        statusBar()->showMessage(action_name + " completed");
    });
}
void MainWindow::ApplySnapshot(const ServiceSnapshot& snapshot) {
    has_snapshot_ = true;
    current_state_ = snapshot.state;

    service_name_value_->setText(snapshot.name);
    state_value_->setText(snapshot.state);
    pid_value_->setText(QString::number(snapshot.pid));
    uptime_value_->setText(FormatUptime(snapshot.uptime_seconds));

    if (snapshot.last_exit_code.has_value()) {
        last_exit_value_->setText(QString::number(*snapshot.last_exit_code));
    } else {
        last_exit_value_->setText("-");
    }

    UpdateActionButtons();
}
void MainWindow::UpdateActionButtons() const {
    const bool service_running = current_state_.compare("Running", Qt::CaseInsensitive) == 0;

    const bool can_control = has_snapshot_ && !action_in_flight_;

    start_button_->setEnabled(can_control && !service_running);

    stop_button_->setEnabled(can_control && service_running);

    restart_button_->setEnabled(can_control && service_running);
}
void MainWindow::ShowBackgroundError(const AgentError& error) const {
    QString message = error.message;

    if (message.isEmpty()) {
        message = "Unknown Agent communication error";
    }

    statusBar()->showMessage("Agent unavailable: " + message);
}
QString MainWindow::FormatUptime(qint64 total_seconds) {
    const qint64 days = total_seconds / 86400;
    const qint64 hours = (total_seconds % 86400) / 3600;
    const qint64 minutes = (total_seconds % 3600) / 60;
    const qint64 seconds = total_seconds % 60;

    if (days > 0) {
        return QString("%1d %2h %3m %4s").arg(days).arg(hours).arg(minutes).arg(seconds);
    }

    return QString("%1h %2m %3s").arg(hours).arg(minutes).arg(seconds);
}
} // namespace aegis::desktop
