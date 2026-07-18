#pragma once

#include "agent/alerts/alert_evaluator.h"
#include "agent/alerts/alert_event.h"

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aegis::agent {
struct AlertManagerOptions {
    std::size_t recent_resolved_capacity{200};
};

struct AlertManagerUpdate {
    std::vector<AlertEvent> created;
    std::vector<AlertEvent> updated;
    std::vector<AlertEvent> resolved;

    std::size_t ignored_invalid_evaluations{0};

    [[nodiscard]] bool HasChanges() const noexcept {
        return !created.empty() || !updated.empty() || !resolved.empty();
    }
};

class AlertManager {
public:
    explicit AlertManager(AlertManagerOptions options = {});

    AlertManagerUpdate ApplyEvaluations(const std::vector<AlertEvaluation>& evaluations);

    [[nodiscard]] std::vector<AlertEvent> GetActiveAlerts() const;

    [[nodiscard]] std::vector<AlertEvent> GetRecentResolvedAlerts(std::size_t limit) const;

    [[nodiscard]] std::vector<AlertEvent> GetAlertsForService(std::string_view service_id, bool include_resolved) const;

    [[nodiscard]] std::optional<AlertEvent> GetAlert(std::string_view alert_id) const;

    [[nodiscard]] bool HasActiveAlert(std::string_view service_id, std::string_view rule_id) const;

    [[nodiscard]] std::optional<AlertEvent> AcknowledgeAndGet(std::string_view alert_id);

    [[nodiscard]] bool Acknowledge(std::string_view alert_id);

    void ForgetService(std::string_view service_id);

    void Clear();

private:
    [[nodiscard]] static std::string MakeAlertEventId(std::string_view service_id, std::string_view rule_id);

    static void SortAlertsForDisplay(std::vector<AlertEvent>& alerts);

    void PushResolvedLocked(AlertEvent event);

private:
    AlertManagerOptions options_;

    mutable std::mutex mutex_;

    std::unordered_map<std::string, AlertEvent> active_alerts_;

    std::deque<AlertEvent> recent_resolved_alerts_;
};
} // namespace aegis::agent
