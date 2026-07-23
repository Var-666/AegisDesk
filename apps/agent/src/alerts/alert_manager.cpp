#include "agent/alerts/alert_manager.h"

#include <algorithm>
#include <limits>
#include <string>

namespace aegis::agent {
namespace {
[[nodiscard]] int SeverityRank(const AlertSeverity severity) noexcept {
    switch (severity) {
        case AlertSeverity::kCritical:
            return 3;
        case AlertSeverity::kWarning:
            return 2;
        case AlertSeverity::kInfo:
            return 1;
    }
    return 0;
}
} // namespace
AlertManager::AlertManager(const AlertManagerOptions options)
    : options_(options) {
    if (options_.recent_resolved_capacity == 0) {
        options_.recent_resolved_capacity = 1;
    }
}
AlertManagerUpdate AlertManager::ApplyEvaluations(const std::vector<AlertEvaluation>& evaluations) {
    AlertManagerUpdate update;

    std::scoped_lock lock(mutex_);

    for (const AlertEvaluation& evaluation : evaluations) {
        if (!evaluation.IsStructurallyValid()) {
            ++update.ignored_invalid_evaluations;
            continue;
        }

        const std::string event_id = MakeAlertEventId(evaluation.service_id, evaluation.rule_id);

        if (!IsValidAlertEventId(event_id)) {
            ++update.ignored_invalid_evaluations;
            continue;
        }

        auto active_iterator = active_alerts_.find(event_id);

        if (evaluation.firing) {
            if (active_iterator == active_alerts_.end()) {
                AlertEvent event{
                    .id = event_id,
                    .service_id = evaluation.service_id,
                    .rule_id = evaluation.rule_id,
                    .severity = evaluation.severity,
                    .state = AlertEventState::kActive,
                    .message = evaluation.message,
                    .first_triggered_at_unix_ms = evaluation.evaluated_at_unix_ms,
                    .last_triggered_at_unix_ms = evaluation.evaluated_at_unix_ms,
                    .resolved_at_unix_ms = std::nullopt,
                    .trigger_count = 1,
                    .acknowledged = false,
                };

                if (!event.IsStructurallyValid()) {
                    ++update.ignored_invalid_evaluations;
                    continue;
                }

                active_alerts_.emplace(event.id, event);
                update.created.push_back(event);

                continue;
            }

            AlertEvent& event = active_iterator->second;

            event.severity = evaluation.severity;
            event.message = evaluation.message;
            event.last_triggered_at_unix_ms = evaluation.evaluated_at_unix_ms;

            if (event.trigger_count < std::numeric_limits<std::uint32_t>::max()) {
                ++event.trigger_count;
            }

            event.state = AlertEventState::kActive;
            event.resolved_at_unix_ms = std::nullopt;

            if (!event.IsStructurallyValid()) {
                ++update.ignored_invalid_evaluations;
                active_alerts_.erase(active_iterator);
                continue;
            }

            update.updated.push_back(event);

            continue;
        }

        if (active_iterator == active_alerts_.end()) {
            continue;
        }

        AlertEvent resolved_event = active_iterator->second;

        resolved_event.state = AlertEventState::kResolved;
        resolved_event.message = evaluation.message;
        resolved_event.resolved_at_unix_ms = evaluation.evaluated_at_unix_ms;

        if (!resolved_event.IsStructurallyValid()) {
            ++update.ignored_invalid_evaluations;
            active_alerts_.erase(active_iterator);
            continue;
        }

        active_alerts_.erase(active_iterator);

        PushResolvedLocked(resolved_event);

        update.resolved.push_back(resolved_event);
    }

    return update;
}
std::vector<AlertEvent> AlertManager::GetActiveAlerts() const {
    std::scoped_lock lock(mutex_);

    std::vector<AlertEvent> result;

    result.reserve(active_alerts_.size());

    for (const auto& [_, event] : active_alerts_) {
        result.push_back(event);
    }

    SortAlertsForDisplay(result);

    return result;
}
std::vector<AlertEvent> AlertManager::GetRecentResolvedAlerts(const std::size_t limit) const {
    std::scoped_lock lock(mutex_);

    std::vector<AlertEvent> result;

    if (limit == 0 || recent_resolved_alerts_.empty()) {
        return result;
    }

    const std::size_t count = std::min(limit, recent_resolved_alerts_.size());

    result.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t reverse_index = recent_resolved_alerts_.size() - 1 - index;

        result.push_back(recent_resolved_alerts_[reverse_index]);
    }

    return result;
}
std::vector<AlertEvent> AlertManager::GetAlertsForService(const std::string_view service_id,
                                                          const bool include_resolved) const {
    std::scoped_lock lock(mutex_);

    std::vector<AlertEvent> result;

    for (const auto& [_, event] : active_alerts_) {
        if (event.service_id == service_id) {
            result.push_back(event);
        }
    }

    if (include_resolved) {
        for (const AlertEvent& event : recent_resolved_alerts_) {
            if (event.service_id == service_id) {
                result.push_back(event);
            }
        }
    }

    SortAlertsForDisplay(result);

    return result;
}
std::optional<AlertEvent> AlertManager::GetAlert(const std::string_view alert_id) const {
    std::scoped_lock lock(mutex_);

    const std::string id(alert_id);

    const auto active_iterator = active_alerts_.find(id);

    if (active_iterator != active_alerts_.end()) {
        return active_iterator->second;
    }

    for (const AlertEvent& event : recent_resolved_alerts_) {
        if (event.id == alert_id) {
            return event;
        }
    }

    return std::nullopt;
}
bool AlertManager::HasActiveAlert(const std::string_view service_id, const std::string_view rule_id) const {
    std::scoped_lock lock(mutex_);

    return active_alerts_.contains(MakeAlertEventId(service_id, rule_id));
}
std::optional<AlertEvent> AlertManager::AcknowledgeAndGet(const std::string_view alert_id) {
    std::scoped_lock lock(mutex_);

    const std::string id(alert_id);

    const auto active_iterator = active_alerts_.find(id);

    if (active_iterator != active_alerts_.end()) {
        active_iterator->second.acknowledged = true;
        return active_iterator->second;
    }

    for (AlertEvent& event : recent_resolved_alerts_) {
        if (event.id == alert_id) {
            event.acknowledged = true;
            return event;
        }
    }

    return std::nullopt;
}
bool AlertManager::Acknowledge(const std::string_view alert_id) {
    return AcknowledgeAndGet(alert_id).has_value();
}
void AlertManager::ForgetService(const std::string_view service_id) {
    std::scoped_lock lock(mutex_);

    for (auto iterator = active_alerts_.begin(); iterator != active_alerts_.end();) {
        if (iterator->second.service_id == service_id) {
            iterator = active_alerts_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    for (auto iterator = recent_resolved_alerts_.begin(); iterator != recent_resolved_alerts_.end();) {
        if (iterator->service_id == service_id) {
            iterator = recent_resolved_alerts_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}
void AlertManager::Clear() {
    std::scoped_lock lock(mutex_);

    active_alerts_.clear();
    recent_resolved_alerts_.clear();
}
std::string AlertManager::MakeAlertEventId(const std::string_view service_id, const std::string_view rule_id) {
    std::string id;

    id.reserve(service_id.size() + rule_id.size() + 1);

    id.append(service_id);
    id.push_back(':');
    id.append(rule_id);

    return id;
}
void AlertManager::SortAlertsForDisplay(std::vector<AlertEvent>& alerts) {
    std::ranges::sort(alerts, [](const AlertEvent& left, const AlertEvent& right) {
        const int left_rank = SeverityRank(left.severity);

        const int right_rank = SeverityRank(right.severity);

        if (left_rank != right_rank) {
            return left_rank > right_rank;
        }

        if (left.state != right.state) {
            return left.state == AlertEventState::kActive;
        }

        if (left.last_triggered_at_unix_ms != right.last_triggered_at_unix_ms) {
            return left.last_triggered_at_unix_ms > right.last_triggered_at_unix_ms;
        }

        if (left.service_id != right.service_id) {
            return left.service_id < right.service_id;
        }

        return left.rule_id < right.rule_id;
    });
}
void AlertManager::PushResolvedLocked(AlertEvent event) {
    recent_resolved_alerts_.push_back(std::move(event));

    while (recent_resolved_alerts_.size() > options_.recent_resolved_capacity) {
        recent_resolved_alerts_.pop_front();
    }
}
} // namespace aegis::agent
