#pragma once

#include "agent/alerts/alert_rule.h"
#include "agent/health/health_status.h"
#include "agent/service/service_definition.h"
#include "agent/metrics/service_metrics.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace aegis::agent {
struct AlertEvaluation {
    std::string service_id;
    std::string rule_id;

    AlertSeverity severity{AlertSeverity::kWarning};

    bool rule_enabled{true};
    bool condition_met{false};
    bool firing{false};

    UnixTimeMilliseconds evaluated_at_unix_ms{0};

    std::optional<UnixTimeMilliseconds> condition_started_at_unix_ms;

    std::string message;

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!IsValidServiceId(service_id)) {
            return false;
        }

        if (!IsValidAlertRuleId(rule_id)) {
            return false;
        }

        if (evaluated_at_unix_ms < 0) {
            return false;
        }

        if (condition_started_at_unix_ms.has_value() && *condition_started_at_unix_ms < 0) {
            return false;
        }

        if (firing && !condition_met) {
            return false;
        }

        if (message.empty()) {
            return false;
        }

        return true;
    }
};

class AlertEvaluator {
public:
    [[nodiscard]] std::vector<AlertEvaluation> Evaluate(const ServiceDefinition& definition,
                                                        const std::optional<ServiceMetrics>& metrics,
                                                        const HealthStatus& health_status,
                                                        UnixTimeMilliseconds evaluated_at_unix_ms);

    void ForgetService(std::string_view service_id);

    void Clear();

private:
    struct RuleRuntimeState {
        std::optional<UnixTimeMilliseconds> condition_started_at_unix_ms;
    };

    struct ConditionResult {
        bool available{false};
        bool condition_met{false};
        std::string observed_value;
        std::string expression;
        std::string message;
    };

    [[nodiscard]] AlertEvaluation EvaluateRule(const AlertRule& rule, const std::optional<ServiceMetrics>& metrics,
                                               const HealthStatus& health_status,
                                               UnixTimeMilliseconds evaluated_at_unix_ms);

    [[nodiscard]] ConditionResult EvaluateCondition(const AlertRule& rule, const std::optional<ServiceMetrics>& metrics,
                                                    const HealthStatus& health_status) const;

    [[nodiscard]] ConditionResult EvaluateNumericCondition(const AlertRule& rule,
                                                           const std::optional<double>& observed_value) const;

    [[nodiscard]] ConditionResult EvaluateHealthCondition(const AlertRule& rule, HealthState observed_state) const;

    [[nodiscard]] static bool CompareDouble(double observed, AlertOperator op, double threshold) noexcept;

    [[nodiscard]] static bool CompareHealthState(HealthState observed, AlertOperator op,
                                                 HealthState threshold) noexcept;

    [[nodiscard]] static std::string MakeRuleKey(std::string_view service_id, std::string_view rule_id);

private:
    std::unordered_map<std::string, RuleRuntimeState> rule_states_;
};
} // namespace aegis::agent