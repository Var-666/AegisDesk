#pragma once

#include "desktop/agent_client.h"

#include <QMainWindow>
#include <QTimer>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace aegis::desktop {
class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QUrl agent_url, QWidget* parent = nullptr);

private:
    using StatusOperation = void (AgentClient::*)(AgentClient::StatusCallback);

    void BuildUI();
    void RefreshAll();
    void RefreshStatus();
    void RefreshLogs();

    void ExecuteAction(const QString& action_name, StatusOperation operation);

    void ApplySnapshot(const ServiceSnapshot& snapshot);

    void UpdateActionButtons() const;

    void ShowBackgroundError(const AgentError& error) const;

    static QString FormatUptime(qint64 total_seconds);

private:
    AgentClient agent_client_;

    QLabel* service_name_value_{nullptr};
    QLabel* state_value_{nullptr};
    QLabel* pid_value_{nullptr};
    QLabel* uptime_value_{nullptr};
    QLabel* last_exit_value_{nullptr};

    QPushButton* start_button_{nullptr};
    QPushButton* stop_button_{nullptr};
    QPushButton* restart_button_{nullptr};
    QPushButton* refresh_logs_button_{nullptr};

    QSpinBox* tail_selector_{nullptr};
    QPlainTextEdit* log_view_{nullptr};

    QTimer refresh_timer_;

    bool has_snapshot_{false};
    bool action_in_flight_{false};
    bool status_request_in_flight_{false};
    bool logs_request_in_flight_{false};

    QString current_state_{"unknown"};

    quint64 status_generation_{0};
    quint64 logs_generation_{0};
};
} // namespace aegis::desktop