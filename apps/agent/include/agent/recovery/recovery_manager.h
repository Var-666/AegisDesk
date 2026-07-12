#pragma once

#include "agent/alerts/alert_event.h"
#include "agent/health/health_status.h"
#include "agent/recovery/recovery_policy.h"
#include "agent/service/process_supervisor.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aegis::agent {

struct RecoveryManagerOptions {
    std::size_t recent_event_capacity{200};
    std::uint32_t suppress_event_cooldown_seconds{30};
};

struct RecoveryManagerUpdate {
    std::vector<RecoveryEvent> events;

    [[nodiscard]] bool HasEvents() const noexcept {
        return !events.empty();
    }
};

class RecoveryManager {
public:
    explicit RecoveryManager(RecoveryManagerOptions options = {});

    RecoveryManagerUpdate EvaluateAndRecover(ProcessSupervisor& supervisor, const HealthStatus& health_status,
                                             const std::vector<AlertEvent>& active_alerts,
                                             UnixTimeMilliseconds now_unix_ms);

    [[nodiscard]] std::vector<RecoveryEvent> GetRecentEvents(std::size_t limit) const;

    [[nodiscard]] std::vector<RecoveryEvent> GetEventsForService(std::string_view service_id, std::size_t limit) const;

    void ForgetService(std::string_view service_id);

    void Clear();

private:
    struct ServiceRecoveryState {
        bool has_seen_healthy{false};

        std::deque<UnixTimeMilliseconds> restart_attempts;

        std::optional<UnixTimeMilliseconds> next_allowed_restart_at_unix_ms;
        std::optional<UnixTimeMilliseconds> startup_grace_until_unix_ms;
        std::optional<UnixTimeMilliseconds> last_suppressed_event_at_unix_ms;
    };

    struct RecoveryTrigger {
        bool triggered{false};

        std::string reason;

        std::optional<std::string> alert_event_id;
    };

    [[nodiscard]] RecoveryTrigger FindTrigger(const ServiceDefinition& definition, const HealthStatus& health_status,
                                              const std::vector<AlertEvent>& active_alerts) const;

    [[nodiscard]] bool IsInStartupGrace(const ServiceRecoveryState& state, UnixTimeMilliseconds now_unix_ms) const;

    [[nodiscard]] bool IsInBackoff(const ServiceRecoveryState& state, UnixTimeMilliseconds now_unix_ms) const;

    [[nodiscard]] bool HasRestartBudgetLocked(ServiceRecoveryState& state, const RecoveryPolicy& policy,
                                              UnixTimeMilliseconds now_unix_ms);

    void RecordRestartAttemptLocked(ServiceRecoveryState& state, const RecoveryPolicy& policy,
                                    UnixTimeMilliseconds now_unix_ms);

    [[nodiscard]] bool ShouldEmitSuppressedEventLocked(ServiceRecoveryState& state, UnixTimeMilliseconds now_unix_ms);

    void PushRecentEventLocked(RecoveryEvent event);

    [[nodiscard]] static UnixTimeMilliseconds SecondsToMilliseconds(std::uint32_t seconds) noexcept;

    [[nodiscard]] static std::size_t CountRestartAttemptsInWindow(const ServiceRecoveryState& state) noexcept;

private:
    RecoveryManagerOptions options_;

    mutable std::mutex mutex_;

    std::unordered_map<std::string, ServiceRecoveryState> states_;

    std::deque<RecoveryEvent> recent_events_;
};

} // namespace aegis::agent