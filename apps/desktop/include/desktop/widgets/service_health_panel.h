#pragma once

#include "desktop/api/agent_client.h"

#include <QGroupBox>
#include <QString>
#include <QtGlobal>

class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace aegis::desktop {

class ServiceHealthPanel final : public QGroupBox {
public:
    explicit ServiceHealthPanel(AgentClient& agent_client, QWidget* parent = nullptr);

    void SetServiceId(const QString& service_id);

    void Refresh();

    void Clear();

private:
    void BuildUi();

    void RefreshHealth();
    void RefreshAlerts();
    void RefreshRecoveryEvents();

    void ApplyHealth(const HealthSnapshot& health) const;
    void ApplyAlerts(const QList<AlertSnapshot>& alerts) const;
    void ApplyRecoveryEvents(const QList<RecoveryEventSnapshot>& events) const;

    void AcknowledgeSelectedAlert();

    void SetStatus(const QString& status) const;

    void UpdateAcknowledgeButton() const;

    [[nodiscard]] QString SelectedAlertId() const;

    [[nodiscard]] static QString FormatTime(qint64 unix_time_milliseconds);

    AgentClient& agent_client_;

    QString service_id_;

    QLabel* panel_status_value_{nullptr};

    QLabel* health_state_value_{nullptr};
    QLabel* health_reason_value_{nullptr};
    QLabel* health_failures_value_{nullptr};
    QLabel* health_checked_at_value_{nullptr};

    QCheckBox* include_resolved_alerts_{nullptr};
    QPushButton* refresh_button_{nullptr};
    QPushButton* acknowledge_button_{nullptr};

    QTableWidget* alerts_table_{nullptr};
    QTableWidget* recovery_events_table_{nullptr};

    bool health_request_in_flight_{false};
    bool alerts_request_in_flight_{false};
    bool recovery_request_in_flight_{false};
    bool ack_request_in_flight_{false};

    quint64 health_generation_{0};
    quint64 alerts_generation_{0};
    quint64 recovery_generation_{0};
};

} // namespace aegis::desktop