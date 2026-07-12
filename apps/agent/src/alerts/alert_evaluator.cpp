#include "agent/alerts/alert_evaluator.h"

#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace aegis::agent {
namespace {
constexpr UnixTimeMilliseconds kMillisecondsPerSecond = 1000;

[[nodiscard]] std::string FormatDouble(const double value) {
    std::ostringstream stream;
    stream << std::setprecision(8) << value;
    return stream.str();
}

[[nodiscard]] std::string FormatRuleExpression(const AlertRule& rule, const std::string& threshold_text) {
    std::ostringstream stream;
    stream << ToString(rule.metric) << ' ' << ToString(rule.op) << ' ' << threshold_text;

    if (rule.duration_seconds > 0) {
        stream << " for " << rule.duration_seconds << "s";
    }

    return stream.str();
}

[[nodiscard]] std::string FormatEvaluationMessage(const std::string& expression, const std::string& observed_value,
                                                  const bool condition_met, const bool firing) {
    std::ostringstream stream;
    stream << expression << ", observed=" << observed_value;

    if (firing) {
        stream << ", firing";
    } else if (condition_met) {
        stream << ", condition met but duration not reached";
    } else {
        stream << ", condition not met";
    }

    return stream.str();
}

[[nodiscard]] std::optional<double> GetNumericMetricValue(const AlertRule& rule,
                                                          const std::optional<ServiceMetrics>& metrics) {
    if (!metrics.has_value()) {
        return std::nullopt;
    }

    if (!metrics->available) {
        return std::nullopt;
    }

    switch (rule.metric) {
        case AlertMetric::kCpuPercent:
            return metrics->cpu_percent;
        case AlertMetric::kRssBytes:
            if (!metrics->rss_bytes.has_value()) {
                return std::nullopt;
            }
            return static_cast<double>(*metrics->rss_bytes);
        case AlertMetric::kThreadCount:
            if (!metrics->thread_count.has_value()) {
                return std::nullopt;
            }
            return static_cast<double>(*metrics->thread_count);
        case AlertMetric::kFdCount:
            if (!metrics->fd_count.has_value()) {
                return std::nullopt;
            }
            return static_cast<double>(*metrics->fd_count);
        case AlertMetric::kHealthState:
            return std::nullopt;
    }
    return std::nullopt;
}
} // namespace
std::vector<AlertEvaluation> AlertEvaluator::Evaluate(const ServiceDefinition& definition,
                                                      const std::optional<ServiceMetrics>& metrics,
                                                      const HealthStatus& health_status,
                                                      const UnixTimeMilliseconds evaluated_at_unix_ms) {
    std::vector<AlertEvaluation> evaluations;
    evaluations.reserve(definition.alert_rules.size());

    for (const AlertRule& rule : definition.alert_rules) {
        evaluations.push_back(EvaluateRule(rule, metrics, health_status, evaluated_at_unix_ms));
    }
    return evaluations;
}

void AlertEvaluator::ForgetService(const std::string_view service_id) {
    const std::string prefix = std::string(service_id) + ":";

    for (auto iterator = rule_states_.begin(); iterator != rule_states_.end();) {
        if (iterator->first.starts_with(prefix)) {
            iterator = rule_states_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void AlertEvaluator::Clear() {
    rule_states_.clear();
}

AlertEvaluation AlertEvaluator::EvaluateRule(const AlertRule& rule, const std::optional<ServiceMetrics>& metrics,
                                             const HealthStatus& health_status,
                                             UnixTimeMilliseconds evaluated_at_unix_ms) {
    AlertEvaluation evaluation{
        .service_id = rule.service_id,
        .rule_id = rule.id,
        .severity = rule.severity,
        .rule_enabled = rule.enabled,
        .condition_met = false,
        .firing = false,
        .evaluated_at_unix_ms = evaluated_at_unix_ms,
        .condition_started_at_unix_ms = std::nullopt,
        .message = "",
    };

    const std::string rule_key = MakeRuleKey(rule.service_id, rule.id);

    if (!rule.enabled) {
        rule_states_.erase(rule_key);
        evaluation.message = "alert rule is disabled";
        return evaluation;
    }

    if (!rule.IsStructurallyValid()) {
        rule_states_.erase(rule_key);
        evaluation.message = "alert rule is structurally invalid";
        return evaluation;
    }

    if (evaluated_at_unix_ms < 0) {
        rule_states_.erase(rule_key);
        evaluation.message = "evaluation time is invalid";
        return evaluation;
    }

    const auto [available, condition_met, observed_value, expression, message] =
        EvaluateCondition(rule, metrics, health_status);

    if (!available) {
        rule_states_.erase(rule_key);
        evaluation.message = message;
        return evaluation;
    }

    evaluation.condition_met = condition_met;

    if (!condition_met) {
        rule_states_.erase(rule_key);
        evaluation.message = FormatEvaluationMessage(expression, observed_value, false, false);
        return evaluation;
    }

    auto& [condition_started_at_unix_ms] = rule_states_[rule_key];

    if (!condition_started_at_unix_ms.has_value() || *condition_started_at_unix_ms > evaluated_at_unix_ms) {
        condition_started_at_unix_ms = evaluated_at_unix_ms;
    }

    evaluation.condition_started_at_unix_ms = condition_started_at_unix_ms;

    const UnixTimeMilliseconds required_duration_ms =
        static_cast<UnixTimeMilliseconds>(rule.duration_seconds) * kMillisecondsPerSecond;
    const UnixTimeMilliseconds elapsed_ms = evaluated_at_unix_ms - *condition_started_at_unix_ms;

    evaluation.firing = elapsed_ms >= required_duration_ms;

    evaluation.message = FormatEvaluationMessage(expression, observed_value, true, evaluation.firing);

    return evaluation;
}
AlertEvaluator::ConditionResult AlertEvaluator::EvaluateCondition(const AlertRule& rule,
                                                                  const std::optional<ServiceMetrics>& metrics,
                                                                  const HealthStatus& health_status) const {
    if (rule.metric == AlertMetric::kHealthState) {
        return EvaluateHealthCondition(rule, health_status.state);
    }

    return EvaluateNumericCondition(rule, GetNumericMetricValue(rule, metrics));
}
AlertEvaluator::ConditionResult
AlertEvaluator::EvaluateNumericCondition(const AlertRule& rule, const std::optional<double>& observed_value) const {
    ConditionResult result;

    if (!rule.numeric_threshold.has_value()) {
        result.message = "numeric alert rule is missing threshold";
        return result;
    }

    const std::string threshold_text = FormatDouble(*rule.numeric_threshold);

    result.expression = FormatRuleExpression(rule, threshold_text);

    if (!observed_value.has_value()) {
        result.message = result.expression + ", metric value is unavailable";
        return result;
    }

    if (!std::isfinite(*observed_value)) {
        result.message = result.expression + ", metric value is not finite";
        return result;
    }

    result.available = true;
    result.observed_value = FormatDouble(*observed_value);
    result.condition_met = CompareDouble(*observed_value, rule.op, *rule.numeric_threshold);

    return result;
}
AlertEvaluator::ConditionResult AlertEvaluator::EvaluateHealthCondition(const AlertRule& rule,
                                                                        const HealthState observed_state) const {
    ConditionResult result;

    if (!rule.health_state_threshold.has_value()) {
        result.message = "health_state alert rule is missing threshold";
        return result;
    }

    const auto threshold_text = std::string(ToString(*rule.health_state_threshold));

    result.expression = FormatRuleExpression(rule, threshold_text);
    result.available = true;
    result.observed_value = std::string(ToString(observed_state));
    result.condition_met = CompareHealthState(observed_state, rule.op, *rule.health_state_threshold);

    return result;
}
bool AlertEvaluator::CompareDouble(const double observed, const AlertOperator op, const double threshold) noexcept {
    switch (op) {
        case AlertOperator::kGreaterThan:
            return observed > threshold;
        case AlertOperator::kGreaterThanOrEqual:
            return observed >= threshold;
        case AlertOperator::kLessThan:
            return observed < threshold;
        case AlertOperator::kLessThanOrEqual:
            return observed <= threshold;
        case AlertOperator::kEqual:
            return observed == threshold;
        case AlertOperator::kNotEqual:
            return observed != threshold;
    }

    return false;
}
bool AlertEvaluator::CompareHealthState(const HealthState observed, const AlertOperator op,
                                        const HealthState threshold) noexcept {
    switch (op) {
        case AlertOperator::kEqual:
            return observed == threshold;
        case AlertOperator::kNotEqual:
            return observed != threshold;
        case AlertOperator::kGreaterThan:
        case AlertOperator::kGreaterThanOrEqual:
        case AlertOperator::kLessThan:
        case AlertOperator::kLessThanOrEqual:
            return false;
    }

    return false;
}
std::string AlertEvaluator::MakeRuleKey(const std::string_view service_id, const std::string_view rule_id) {
    std::string key;

    key.reserve(service_id.size() + rule_id.size() + 1);

    key.append(service_id);
    key.push_back(':');
    key.append(rule_id);

    return key;
}
} // namespace aegis::agent