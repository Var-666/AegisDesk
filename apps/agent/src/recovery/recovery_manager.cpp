#include "agent/recovery/recovery_manager.h"

#include <string>

namespace aegis::agent {
namespace {

constexpr UnixTimeMilliseconds kMillisecondsPerSecond = 1000;

} // namespace

RecoveryManager::RecoveryManager(const RecoveryManagerOptions options)
    : options_(options) {
    if (options_.recent_event_capacity == 0) {
        options_.recent_event_capacity = 1;
    }

    if (options_.suppress_event_cooldown_seconds == 0) {
        options_.suppress_event_cooldown_seconds = 1;
    }
}

RecoveryManagerUpdate RecoveryManager::EvaluateAndRecover(ProcessSupervisor& supervisor,
                                                          const HealthStatus& health_status,
                                                          const std::vector<AlertEvent>& active_alerts,
                                                          const UnixTimeMilliseconds now_unix_ms) {
    RecoveryManagerUpdate update;

    const ServiceDefinition& definition = supervisor.Definition();

    const RecoveryPolicy& policy = definition.recovery_policy;

    std::scoped_lock lock(mutex_);

    ServiceRecoveryState& state = states_[definition.id];

    if (health_status.state == HealthState::kHealthy) {
        state.has_seen_healthy = true;
    }

    if (!policy.enabled || !policy.IsStructurallyValid()) {
        return update;
    }

    const RecoveryTrigger trigger = FindTrigger(definition, health_status, active_alerts);

    if (!trigger.triggered) {
        return update;
    }

    const ServiceStatus current_status = supervisor.GetStatus();

    if (IsDesiredStopped(current_status.desired_state)) {
        return update;
    }

    if (health_status.state == HealthState::kUnhealthy && !state.has_seen_healthy && !definition.auto_start) {
        if (ShouldEmitSuppressedEventLocked(state, now_unix_ms)) {
            RecoveryEvent event{
                .service_id = definition.id,
                .type = RecoveryEventType::kRestartSuppressed,
                .occurred_at_unix_ms = now_unix_ms,
                .reason = "auto recovery suppressed because service has not been observed healthy yet",
                .alert_event_id = trigger.alert_event_id,
                .restart_count_in_window = static_cast<std::uint32_t>(CountRestartAttemptsInWindow(state)),
            };

            if (event.IsStructurallyValid()) {
                PushRecentEventLocked(event);
                update.events.push_back(event);
            }
        }

        return update;
    }

    if (IsInStartupGrace(state, now_unix_ms)) {
        if (ShouldEmitSuppressedEventLocked(state, now_unix_ms)) {
            RecoveryEvent event{
                .service_id = definition.id,
                .type = RecoveryEventType::kRestartSuppressed,
                .occurred_at_unix_ms = now_unix_ms,
                .reason = "auto recovery suppressed during startup grace period",
                .alert_event_id = trigger.alert_event_id,
                .restart_count_in_window = static_cast<std::uint32_t>(CountRestartAttemptsInWindow(state)),
            };

            if (event.IsStructurallyValid()) {
                PushRecentEventLocked(event);
                update.events.push_back(event);
            }
        }

        return update;
    }

    if (IsInBackoff(state, now_unix_ms)) {
        if (ShouldEmitSuppressedEventLocked(state, now_unix_ms)) {
            RecoveryEvent event{
                .service_id = definition.id,
                .type = RecoveryEventType::kRestartSuppressed,
                .occurred_at_unix_ms = now_unix_ms,
                .reason = "auto recovery suppressed by backoff window",
                .alert_event_id = trigger.alert_event_id,
                .restart_count_in_window = static_cast<std::uint32_t>(CountRestartAttemptsInWindow(state)),
            };

            if (event.IsStructurallyValid()) {
                PushRecentEventLocked(event);
                update.events.push_back(event);
            }
        }

        return update;
    }

    if (!HasRestartBudgetLocked(state, policy, now_unix_ms)) {
        if (ShouldEmitSuppressedEventLocked(state, now_unix_ms)) {
            RecoveryEvent event{
                .service_id = definition.id,
                .type = RecoveryEventType::kRestartSuppressed,
                .occurred_at_unix_ms = now_unix_ms,
                .reason = "auto recovery suppressed because restart limit "
                          "was reached",
                .alert_event_id = trigger.alert_event_id,
                .restart_count_in_window = static_cast<std::uint32_t>(CountRestartAttemptsInWindow(state)),
            };

            if (event.IsStructurallyValid()) {
                PushRecentEventLocked(event);
                update.events.push_back(event);
            }
        }

        return update;
    }

    {
        RecoveryEvent event{
            .service_id = definition.id,
            .type = RecoveryEventType::kRestartStarted,
            .occurred_at_unix_ms = now_unix_ms,
            .reason = trigger.reason,
            .alert_event_id = trigger.alert_event_id,
            .restart_count_in_window = static_cast<std::uint32_t>(CountRestartAttemptsInWindow(state)),
        };

        if (event.IsStructurallyValid()) {
            PushRecentEventLocked(event);
            update.events.push_back(event);
        }
    }

    std::string operation_error;

    bool succeeded = false;

    if (current_status.state == ServiceState::kRunning && current_status.pid > 0) {
        succeeded = supervisor.Restart(operation_error);
    } else {
        succeeded = supervisor.Start(operation_error);
    }

    RecordRestartAttemptLocked(state, policy, now_unix_ms);

    if (succeeded) {
        RecoveryEvent event{
            .service_id = definition.id,
            .type = RecoveryEventType::kRestartSucceeded,
            .occurred_at_unix_ms = now_unix_ms,
            .reason = trigger.reason,
            .alert_event_id = trigger.alert_event_id,
            .restart_count_in_window = static_cast<std::uint32_t>(CountRestartAttemptsInWindow(state)),
        };

        if (event.IsStructurallyValid()) {
            PushRecentEventLocked(event);
            update.events.push_back(event);
        }

        return update;
    }

    RecoveryEvent event{
        .service_id = definition.id,
        .type = RecoveryEventType::kRestartFailed,
        .occurred_at_unix_ms = now_unix_ms,
        .reason = operation_error.empty() ? "restart operation failed" : operation_error,
        .alert_event_id = trigger.alert_event_id,
        .restart_count_in_window = static_cast<std::uint32_t>(CountRestartAttemptsInWindow(state)),
    };

    if (event.IsStructurallyValid()) {
        PushRecentEventLocked(event);
        update.events.push_back(event);
    }

    return update;
}

std::vector<RecoveryEvent> RecoveryManager::GetRecentEvents(const std::size_t limit) const {
    std::scoped_lock lock(mutex_);

    std::vector<RecoveryEvent> result;

    if (limit == 0 || recent_events_.empty()) {
        return result;
    }

    const std::size_t count = std::min(limit, recent_events_.size());

    result.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t reverse_index = recent_events_.size() - 1 - index;

        result.push_back(recent_events_[reverse_index]);
    }

    return result;
}

std::vector<RecoveryEvent> RecoveryManager::GetEventsForService(const std::string_view service_id,
                                                                const std::size_t limit) const {
    std::scoped_lock lock(mutex_);

    std::vector<RecoveryEvent> result;

    if (limit == 0) {
        return result;
    }

    for (std::size_t index = 0; index < recent_events_.size() && result.size() < limit; ++index) {
        const std::size_t reverse_index = recent_events_.size() - 1 - index;

        const RecoveryEvent& event = recent_events_[reverse_index];

        if (event.service_id == service_id) {
            result.push_back(event);
        }
    }

    return result;
}

void RecoveryManager::ForgetService(const std::string_view service_id) {
    std::scoped_lock lock(mutex_);

    states_.erase(std::string(service_id));

    for (auto iterator = recent_events_.begin(); iterator != recent_events_.end();) {
        if (iterator->service_id == service_id) {
            iterator = recent_events_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void RecoveryManager::Clear() {
    std::scoped_lock lock(mutex_);

    states_.clear();
    recent_events_.clear();
}

RecoveryManager::RecoveryTrigger RecoveryManager::FindTrigger(const ServiceDefinition& definition,
                                                              const HealthStatus& health_status,
                                                              const std::vector<AlertEvent>& active_alerts) const {
    const RecoveryPolicy& policy = definition.recovery_policy;

    if (policy.restart_on_unhealthy && health_status.state == HealthState::kUnhealthy) {
        return RecoveryTrigger{
            .triggered = true,
            .reason = "service health state is unhealthy: " + health_status.reason,
            .alert_event_id = std::nullopt,
        };
    }

    if (policy.restart_on_critical_alert) {
        for (const AlertEvent& alert : active_alerts) {
            if (alert.service_id == definition.id && alert.state == AlertEventState::kActive
                && alert.severity == AlertSeverity::kCritical) {
                return RecoveryTrigger{
                    .triggered = true,
                    .reason = "critical alert is active: " + alert.id,
                    .alert_event_id = alert.id,
                };
            }
        }
    }

    return RecoveryTrigger{};
}

bool RecoveryManager::IsInStartupGrace(const ServiceRecoveryState& state,
                                       const UnixTimeMilliseconds now_unix_ms) const {
    return state.startup_grace_until_unix_ms.has_value() && now_unix_ms < *state.startup_grace_until_unix_ms;
}

bool RecoveryManager::IsInBackoff(const ServiceRecoveryState& state, const UnixTimeMilliseconds now_unix_ms) const {
    return state.next_allowed_restart_at_unix_ms.has_value() && now_unix_ms < *state.next_allowed_restart_at_unix_ms;
}

bool RecoveryManager::HasRestartBudgetLocked(ServiceRecoveryState& state, const RecoveryPolicy& policy,
                                             const UnixTimeMilliseconds now_unix_ms) {
    const UnixTimeMilliseconds window_ms = SecondsToMilliseconds(policy.window_seconds);

    while (!state.restart_attempts.empty() && state.restart_attempts.front() + window_ms < now_unix_ms) {
        state.restart_attempts.pop_front();
    }

    return state.restart_attempts.size() < policy.max_restarts;
}

void RecoveryManager::RecordRestartAttemptLocked(ServiceRecoveryState& state, const RecoveryPolicy& policy,
                                                 const UnixTimeMilliseconds now_unix_ms) {
    state.restart_attempts.push_back(now_unix_ms);

    state.next_allowed_restart_at_unix_ms = now_unix_ms + SecondsToMilliseconds(policy.backoff_seconds);

    state.startup_grace_until_unix_ms = now_unix_ms + SecondsToMilliseconds(policy.startup_grace_seconds);
}

bool RecoveryManager::ShouldEmitSuppressedEventLocked(ServiceRecoveryState& state,
                                                      const UnixTimeMilliseconds now_unix_ms) const {
    const UnixTimeMilliseconds cooldown_ms = SecondsToMilliseconds(options_.suppress_event_cooldown_seconds);

    if (!state.last_suppressed_event_at_unix_ms.has_value()
        || now_unix_ms >= *state.last_suppressed_event_at_unix_ms + cooldown_ms) {
        state.last_suppressed_event_at_unix_ms = now_unix_ms;

        return true;
    }

    return false;
}

void RecoveryManager::PushRecentEventLocked(RecoveryEvent event) {
    recent_events_.push_back(std::move(event));

    while (recent_events_.size() > options_.recent_event_capacity) {
        recent_events_.pop_front();
    }
}

UnixTimeMilliseconds RecoveryManager::SecondsToMilliseconds(const std::uint32_t seconds) noexcept {
    return static_cast<UnixTimeMilliseconds>(seconds) * kMillisecondsPerSecond;
}

std::size_t RecoveryManager::CountRestartAttemptsInWindow(const ServiceRecoveryState& state) noexcept {
    return state.restart_attempts.size();
}

} // namespace aegis::agent
