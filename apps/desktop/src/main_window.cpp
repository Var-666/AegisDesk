#include "desktop/main_window.h"

#include <QAbstractItemView>
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
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace aegis::desktop {
namespace {

constexpr int kRefreshIntervalMs = 1000;

constexpr int kServiceIdRole = Qt::UserRole;

QLabel* CreateValueLabel(QWidget* parent) {
    auto* label = new QLabel("-", parent);

    label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    return label;
}

QString ServiceListText(const ServiceSnapshot& service) {
    return QString("%1\n%2  [%3]").arg(service.display_name, service.id, service.state);
}

} // namespace

MainWindow::MainWindow(QUrl agent_url, QWidget* parent)
    : QMainWindow(parent)
    , agent_client_(std::move(agent_url)) {
    BuildUi();

    setWindowTitle("AegisDesk - Service Console");
    resize(1180, 760);

    statusBar()->showMessage("Agent: " + agent_client_.BaseUrl().toString());

    connect(&refresh_timer_, &QTimer::timeout, this, &MainWindow::RefreshAll);

    refresh_timer_.start(kRefreshIntervalMs);

    RefreshAll();
}

void MainWindow::BuildUi() {
    auto* central_widget = new QWidget(this);
    auto* root_layout = new QVBoxLayout(central_widget);

    auto* splitter = new QSplitter(Qt::Horizontal, central_widget);

    auto* services_group = new QGroupBox("Services", splitter);

    auto* services_layout = new QVBoxLayout(services_group);

    service_list_ = new QListWidget(services_group);

    service_list_->setSelectionMode(QAbstractItemView::SingleSelection);

    service_list_->setMinimumWidth(260);

    services_layout->addWidget(service_list_);

    splitter->addWidget(services_group);

    auto* detail_panel = new QWidget(splitter);
    auto* detail_layout = new QVBoxLayout(detail_panel);

    details_group_ = new QGroupBox("Service Details", detail_panel);

    auto* service_layout = new QGridLayout(details_group_);

    service_layout->addWidget(new QLabel("Display Name:", details_group_), 0, 0);

    service_name_value_ = CreateValueLabel(details_group_);

    service_layout->addWidget(service_name_value_, 0, 1);

    service_layout->addWidget(new QLabel("Service ID:", details_group_), 0, 2);

    service_id_value_ = CreateValueLabel(details_group_);

    service_layout->addWidget(service_id_value_, 0, 3);

    service_layout->addWidget(new QLabel("State:", details_group_), 1, 0);

    state_value_ = CreateValueLabel(details_group_);

    service_layout->addWidget(state_value_, 1, 1);

    service_layout->addWidget(new QLabel("PID:", details_group_), 1, 2);

    pid_value_ = CreateValueLabel(details_group_);

    service_layout->addWidget(pid_value_, 1, 3);

    service_layout->addWidget(new QLabel("Uptime:", details_group_), 2, 0);

    uptime_value_ = CreateValueLabel(details_group_);

    service_layout->addWidget(uptime_value_, 2, 1);

    service_layout->addWidget(new QLabel("Auto Start:", details_group_), 2, 2);

    auto_start_value_ = CreateValueLabel(details_group_);

    service_layout->addWidget(auto_start_value_, 2, 3);

    service_layout->addWidget(new QLabel("Last Exit:", details_group_), 3, 0);

    last_exit_value_ = CreateValueLabel(details_group_);

    service_layout->addWidget(last_exit_value_, 3, 1);

    auto* actions_layout = new QHBoxLayout();

    start_button_ = new QPushButton("Start", details_group_);

    stop_button_ = new QPushButton("Stop", details_group_);

    restart_button_ = new QPushButton("Restart", details_group_);

    actions_layout->addWidget(start_button_);
    actions_layout->addWidget(stop_button_);
    actions_layout->addWidget(restart_button_);
    actions_layout->addStretch();

    service_layout->addLayout(actions_layout, 4, 0, 1, 4);

    detail_layout->addWidget(details_group_);

    auto* logs_group = new QGroupBox("Recent Logs", detail_panel);

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

    detail_layout->addWidget(logs_group, 1);

    splitter->addWidget(detail_panel);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    root_layout->addWidget(splitter);

    setCentralWidget(central_widget);

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

    ClearCurrentServiceDetails();
}

void MainWindow::RefreshAll() {
    RefreshServices();
    RefreshStatus();
    RefreshLogs();
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

    status_request_in_flight_ = false;
    logs_request_in_flight_ = false;

    ClearCurrentServiceDetails();

    statusBar()->showMessage("Selected service: " + current_service_id_);

    RefreshStatus();
    RefreshLogs();
}

void MainWindow::ExecuteAction(const QString& action_name, const StatusOperation operation) {
    if (action_in_flight_ || current_service_id_.isEmpty()) {
        return;
    }

    const QString service_id = current_service_id_;

    action_in_flight_ = true;

    ++status_generation_;
    ++logs_generation_;

    status_request_in_flight_ = false;
    logs_request_in_flight_ = false;

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

void MainWindow::ClearCurrentServiceDetails() {
    has_snapshot_ = false;
    current_state_ = "unknown";

    details_group_->setTitle("Service Details");

    service_name_value_->setText("-");
    service_id_value_->setText("-");
    state_value_->setText("-");
    pid_value_->setText("-");
    uptime_value_->setText("-");
    auto_start_value_->setText("-");
    last_exit_value_->setText("-");

    log_view_->clear();

    UpdateActionButtons();
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

} // namespace aegis::desktop