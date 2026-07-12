#pragma once

#include "desktop/api/agent_client.h"

#include <QMainWindow>
#include <QTimer>

class QGroupBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QWidget;

namespace aegis::desktop {

class MetricsHistoryPanel;
class ServiceHealthPanel;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QUrl agent_url, QWidget* parent = nullptr);

private:
    using StatusOperation = void (AgentClient::*)(const QString&, AgentClient::StatusCallback);

    void BuildUi();
    [[nodiscard]] QWidget* BuildServiceListPanel(QWidget* parent);
    [[nodiscard]] QWidget* BuildDetailPanel(QWidget* parent);
    [[nodiscard]] QWidget* BuildOverviewTab(QWidget* parent);
    [[nodiscard]] QWidget* BuildHealthTab(QWidget* parent);
    [[nodiscard]] QWidget* BuildTrendsTab(QWidget* parent);
    [[nodiscard]] QGroupBox* BuildServiceDetailsGroup(QWidget* parent);
    [[nodiscard]] QGroupBox* BuildMetricsGroup(QWidget* parent);
    [[nodiscard]] QGroupBox* BuildLogsGroup(QWidget* parent);
    void ConnectUiSignals();

    void RefreshAll();
    void RefreshServices();
    void RefreshStatus();
    void RefreshLogs();
    void RefreshMetrics();

    void ApplyServiceList(const QList<ServiceSnapshot>& services);

    void SelectService(const QString& service_id);

    void ExecuteAction(const QString& action_name, StatusOperation operation);

    void ApplySnapshot(const ServiceSnapshot& snapshot);

    void ApplyMetrics(const ServiceMetricsSnapshot& metrics);

    void ClearCurrentServiceDetails();
    void ClearMetrics();
    void ShowMetricsNotReady();

    void UpdateActionButtons();

    void ShowBackgroundError(const AgentError& error);

    [[nodiscard]] static QString FormatUptime(qint64 total_seconds);

    [[nodiscard]] static QString FormatBytes(quint64 bytes);

    [[nodiscard]] static QString FormatSampleTime(qint64 unix_time_milliseconds);

    AgentClient agent_client_;

    QListWidget* service_list_{nullptr};

    QGroupBox* details_group_{nullptr};
    MetricsHistoryPanel* metrics_history_panel_{nullptr};
    ServiceHealthPanel* service_health_panel_{nullptr};

    QLabel* service_name_value_{nullptr};
    QLabel* service_id_value_{nullptr};
    QLabel* state_value_{nullptr};
    QLabel* pid_value_{nullptr};
    QLabel* uptime_value_{nullptr};
    QLabel* auto_start_value_{nullptr};
    QLabel* last_exit_value_{nullptr};

    QLabel* metrics_status_value_{nullptr};
    QLabel* cpu_value_{nullptr};
    QLabel* rss_value_{nullptr};
    QLabel* thread_count_value_{nullptr};
    QLabel* fd_count_value_{nullptr};
    QLabel* last_sample_value_{nullptr};

    QPushButton* start_button_{nullptr};
    QPushButton* stop_button_{nullptr};
    QPushButton* restart_button_{nullptr};
    QPushButton* refresh_logs_button_{nullptr};

    QSpinBox* tail_selector_{nullptr};
    QPlainTextEdit* log_view_{nullptr};

    QTimer refresh_timer_;

    bool has_snapshot_{false};
    bool action_in_flight_{false};
    bool services_request_in_flight_{false};
    bool status_request_in_flight_{false};
    bool logs_request_in_flight_{false};
    bool metrics_request_in_flight_{false};

    QString current_service_id_;
    QString current_state_{"unknown"};

    quint64 services_generation_{0};
    quint64 status_generation_{0};
    quint64 logs_generation_{0};
    quint64 metrics_generation_{0};
};

} // namespace aegis::desktop
