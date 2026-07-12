#include "desktop/widgets/service_health_panel.h"
#include "desktop/common/ui_helpers.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace aegis::desktop {
namespace {

constexpr int kAlertIdRole = Qt::UserRole;
constexpr int kRecoveryEventLimit = 50;

} // namespace

ServiceHealthPanel::ServiceHealthPanel(AgentClient& agent_client, QWidget* parent)
    : QGroupBox("Health / Alerts / Recovery", parent)
    , agent_client_(agent_client) {
    BuildUi();
    Clear();
}

void ServiceHealthPanel::BuildUi() {
    auto* root_layout = new QVBoxLayout(this);

    // 顶部状态栏
    auto* status_layout = new QHBoxLayout();
    status_layout->addWidget(new QLabel("Panel Status:", this));
    panel_status_value_ = ui::CreateValueLabel(this, true);
    status_layout->addWidget(panel_status_value_);
    status_layout->addStretch();
    root_layout->addLayout(status_layout);

    // Health Status 区域
    auto* health_group = new QGroupBox("Health Status", this);
    auto* health_layout = new QGridLayout(health_group);

    ui::AddValueRow(health_layout, 0, 0, "State:", health_state_value_, health_group);
    ui::AddValueRow(health_layout, 1, 0, "Reason:", health_reason_value_, health_group);
    ui::AddValueRow(health_layout, 2, 0, "Consecutive Failures:", health_failures_value_, health_group);
    ui::AddValueRow(health_layout, 3, 0, "Last Check:", health_checked_at_value_, health_group);

    root_layout->addWidget(health_group);

    // 工具栏
    auto* toolbar_layout = new QHBoxLayout();

    include_resolved_alerts_ = new QCheckBox("Include resolved alerts", this);

    refresh_button_ = new QPushButton("Refresh Health", this);

    acknowledge_button_ = new QPushButton("Acknowledge Selected Alert", this);

    toolbar_layout->addWidget(include_resolved_alerts_);
    toolbar_layout->addWidget(refresh_button_);
    toolbar_layout->addWidget(acknowledge_button_);
    toolbar_layout->addStretch();

    root_layout->addLayout(toolbar_layout);

    // Alerts 表格
    alerts_table_ = new QTableWidget(this);

    ui::ConfigureReadOnlyTable(alerts_table_, {"Severity", "State", "Rule", "Message", "Triggered", "Resolved", "Ack"},
                               170);

    root_layout->addWidget(new QLabel("Alerts:", this));
    root_layout->addWidget(alerts_table_);

    // Recovery Events 表格
    recovery_events_table_ = new QTableWidget(this);

    ui::ConfigureReadOnlyTable(recovery_events_table_, {"Type", "Reason", "Occurred", "Alert", "Restarts"}, 140);

    root_layout->addWidget(new QLabel("Recovery Events:", this));
    root_layout->addWidget(recovery_events_table_);

    connect(refresh_button_, &QPushButton::clicked, this, &ServiceHealthPanel::Refresh);
    connect(include_resolved_alerts_, &QCheckBox::toggled, this, [this] { RefreshAlerts(); });
    connect(acknowledge_button_, &QPushButton::clicked, this, &ServiceHealthPanel::AcknowledgeSelectedAlert);
    connect(alerts_table_, &QTableWidget::itemSelectionChanged, this, &ServiceHealthPanel::UpdateAcknowledgeButton);
}

void ServiceHealthPanel::SetServiceId(const QString& service_id) {
    if (service_id == service_id_) {
        return;
    }

    service_id_ = service_id;

    ++health_generation_;
    ++alerts_generation_;
    ++recovery_generation_;

    health_request_in_flight_ = false;
    alerts_request_in_flight_ = false;
    recovery_request_in_flight_ = false;
    ack_request_in_flight_ = false;

    health_state_value_->setText("-");
    health_reason_value_->setText("-");
    health_failures_value_->setText("-");
    health_checked_at_value_->setText("-");

    alerts_table_->setRowCount(0);
    recovery_events_table_->setRowCount(0);

    SetStatus(service_id_.isEmpty() ? "No service selected" : "Loading health information...");

    UpdateAcknowledgeButton();

    Refresh();
}

void ServiceHealthPanel::Refresh() {
    RefreshHealth();
    RefreshAlerts();
    RefreshRecoveryEvents();
}

void ServiceHealthPanel::Clear() {
    service_id_.clear();

    ++health_generation_;
    ++alerts_generation_;
    ++recovery_generation_;

    health_request_in_flight_ = false;
    alerts_request_in_flight_ = false;
    recovery_request_in_flight_ = false;
    ack_request_in_flight_ = false;

    health_state_value_->setText("-");
    health_reason_value_->setText("-");
    health_failures_value_->setText("-");
    health_checked_at_value_->setText("-");

    alerts_table_->setRowCount(0);
    recovery_events_table_->setRowCount(0);

    SetStatus("No service selected");

    UpdateAcknowledgeButton();
}

void ServiceHealthPanel::RefreshHealth() {
    if (service_id_.isEmpty() || health_request_in_flight_) {
        return;
    }

    health_request_in_flight_ = true;

    const QString requested_service_id = service_id_;
    const quint64 generation = ++health_generation_;

    agent_client_.GetHealth(requested_service_id, [this, requested_service_id,
                                                   generation](std::optional<HealthSnapshot> health, AgentError error) {
        if (generation != health_generation_ || requested_service_id != service_id_) {
            return;
        }

        health_request_in_flight_ = false;

        if (!health.has_value()) {
            SetStatus("Health unavailable: " + error.message);

            return;
        }

        ApplyHealth(*health);
    });
}

void ServiceHealthPanel::RefreshAlerts() {
    if (service_id_.isEmpty() || alerts_request_in_flight_) {
        return;
    }

    alerts_request_in_flight_ = true;

    const QString requested_service_id = service_id_;
    const quint64 generation = ++alerts_generation_;

    agent_client_.GetServiceAlerts(
        requested_service_id, include_resolved_alerts_->isChecked(),
        [this, requested_service_id, generation](std::optional<QList<AlertSnapshot>> alerts, AgentError error) {
            if (generation != alerts_generation_ || requested_service_id != service_id_) {
                return;
            }

            alerts_request_in_flight_ = false;

            if (!alerts.has_value()) {
                SetStatus("Alerts unavailable: " + error.message);

                return;
            }

            ApplyAlerts(*alerts);
        });
}

void ServiceHealthPanel::RefreshRecoveryEvents() {
    if (service_id_.isEmpty() || recovery_request_in_flight_) {
        return;
    }

    recovery_request_in_flight_ = true;

    const QString requested_service_id = service_id_;

    const quint64 generation = ++recovery_generation_;

    agent_client_.GetServiceRecoveryEvents(
        requested_service_id, kRecoveryEventLimit,
        [this, requested_service_id, generation](std::optional<QList<RecoveryEventSnapshot>> events, AgentError error) {
            if (generation != recovery_generation_ || requested_service_id != service_id_) {
                return;
            }

            recovery_request_in_flight_ = false;

            if (!events.has_value()) {
                SetStatus("Recovery events unavailable: " + error.message);

                return;
            }

            ApplyRecoveryEvents(*events);
        });
}

void ServiceHealthPanel::ApplyHealth(const HealthSnapshot& health) const {
    health_state_value_->setText(health.state);
    health_reason_value_->setText(health.reason);

    health_failures_value_->setText(QString::number(health.consecutive_failures));

    health_checked_at_value_->setText(FormatTime(health.checked_at_unix_ms));

    SetStatus("Health information updated");
}

void ServiceHealthPanel::ApplyAlerts(const QList<AlertSnapshot>& alerts) const {
    alerts_table_->setRowCount(alerts.size());

    for (int row = 0; row < alerts.size(); ++row) {
        const AlertSnapshot& alert = alerts[row];

        auto* severity_item = ui::MakeReadOnlyItem(alert.severity);

        severity_item->setData(kAlertIdRole, alert.id);

        alerts_table_->setItem(row, 0, severity_item);
        alerts_table_->setItem(row, 1, ui::MakeReadOnlyItem(alert.state));
        alerts_table_->setItem(row, 2, ui::MakeReadOnlyItem(alert.rule_id));
        alerts_table_->setItem(row, 3, ui::MakeReadOnlyItem(alert.message));
        alerts_table_->setItem(row, 4, ui::MakeReadOnlyItem(FormatTime(alert.first_triggered_at_unix_ms)));
        alerts_table_->setItem(
            row, 5,
            ui::MakeReadOnlyItem(alert.resolved_at_unix_ms.has_value() ? FormatTime(*alert.resolved_at_unix_ms) : "-"));
        alerts_table_->setItem(row, 6, ui::MakeReadOnlyItem(alert.acknowledged ? "yes" : "no"));
    }

    alerts_table_->resizeRowsToContents();

    UpdateAcknowledgeButton();
}

void ServiceHealthPanel::ApplyRecoveryEvents(const QList<RecoveryEventSnapshot>& events) const {
    recovery_events_table_->setRowCount(events.size());

    for (int row = 0; row < events.size(); ++row) {
        const RecoveryEventSnapshot& event = events[row];

        recovery_events_table_->setItem(row, 0, ui::MakeReadOnlyItem(event.type));
        recovery_events_table_->setItem(row, 1, ui::MakeReadOnlyItem(event.reason));
        recovery_events_table_->setItem(row, 2, ui::MakeReadOnlyItem(FormatTime(event.occurred_at_unix_ms)));
        recovery_events_table_->setItem(
            row, 3, ui::MakeReadOnlyItem(event.alert_event_id.has_value() ? *event.alert_event_id : "-"));
        recovery_events_table_->setItem(row, 4, ui::MakeReadOnlyItem(QString::number(event.restart_count_in_window)));
    }

    recovery_events_table_->resizeRowsToContents();
}

void ServiceHealthPanel::AcknowledgeSelectedAlert() {
    if (ack_request_in_flight_) {
        return;
    }

    const QString alert_id = SelectedAlertId();

    if (alert_id.isEmpty()) {
        return;
    }

    ack_request_in_flight_ = true;

    agent_client_.AcknowledgeAlert(alert_id, [this](std::optional<AlertSnapshot> alert, AgentError error) {
        ack_request_in_flight_ = false;

        if (!alert.has_value()) {
            SetStatus("Acknowledge failed: " + error.message);

            UpdateAcknowledgeButton();

            return;
        }

        SetStatus("Alert acknowledged: " + alert->id);

        RefreshAlerts();
        UpdateAcknowledgeButton();
    });

    UpdateAcknowledgeButton();
}

void ServiceHealthPanel::SetStatus(const QString& status) const {
    panel_status_value_->setText(status);
}

void ServiceHealthPanel::UpdateAcknowledgeButton() const {
    acknowledge_button_->setEnabled(!service_id_.isEmpty() && !ack_request_in_flight_ && !SelectedAlertId().isEmpty());
}

QString ServiceHealthPanel::SelectedAlertId() const {
    const QList<QTableWidgetItem*> selected_items = alerts_table_->selectedItems();

    if (selected_items.isEmpty()) {
        return {};
    }

    const int row = selected_items.front()->row();

    QTableWidgetItem* item = alerts_table_->item(row, 0);

    if (item == nullptr) {
        return {};
    }

    return item->data(kAlertIdRole).toString();
}

QString ServiceHealthPanel::FormatTime(const qint64 unix_time_milliseconds) {
    return ui::FormatLocalDateTime(unix_time_milliseconds);
}

} // namespace aegis::desktop
