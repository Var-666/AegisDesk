#include "../include/agent/service/service_registry.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <sstream>

namespace aegis::agent {
namespace {
using boost::property_tree::ptree;

constexpr int kSupportedSchemaVersion = 1;

[[nodiscard]] std::filesystem::path ResolvePath(const std::filesystem::path& base_dir,
                                                const std::filesystem::path& value) {
    if (value.is_absolute()) {
        return value.lexically_normal();
    }

    return (base_dir / value).lexically_normal();
}

[[nodiscard]] std::string Join(const std::vector<std::string>& values, const std::string_view separator) {
    std::ostringstream output;

    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            output << separator;
        }
        output << values[index];
    }
    return output.str();
}

bool ReadRequiredString(const ptree& object, const std::string_view field_name, const std::string_view context,
                        std::string& value, std::string& error) {
    const auto field = object.get_optional<std::string>(std::string(field_name));

    if (!field.has_value() || field->empty()) {
        error = std::string(context) + "." + std::string(field_name) + " must be a non-empty string";
        return false;
    }

    value = *field;
    return true;
}

bool ReadArgs(const ptree& object, const std::string_view context, std::vector<std::string>& args, std::string& error) {
    const auto args_node = object.get_child_optional("args");

    if (!args_node.has_value()) {
        return true;
    }

    // JSON 标量会被 PropertyTree 解析为 data()；
    // 真正数组的 data() 应为空。
    if (!args_node->data().empty()) {
        error = std::string(context) + ".args must be a JSON array";
        return false;
    }

    for (const auto& [key, item] : *args_node) {
        // JSON 数组在 PropertyTree 中的 key 应为空。
        if (!key.empty()) {
            error = std::string(context) + ".args must be a JSON array";
            return false;
        }

        if (!item.empty()) {
            error = std::string(context) + ".args items must be strings";
            return false;
        }

        const auto argument = item.get_value_optional<std::string>();

        if (!argument.has_value()) {
            error = std::string(context) + ".args items must be strings";
            return false;
        }

        args.push_back(*argument);
    }

    return true;
}

bool ReadAutoStart(const ptree& object, const std::string_view context, bool& auto_start, std::string& error) {
    const auto auto_start_node = object.get_child_optional("auto_start");

    if (!auto_start_node.has_value()) {
        auto_start = false;
        return true;
    }

    const auto value = object.get_optional<bool>("auto_start");

    if (!value.has_value()) {
        error = std::string(context) + ".auto_start must be true or false";
        return false;
    }

    auto_start = *value;
    return true;
}

[[nodiscard]] bool ParseHealthCheckType(const std::string& value, HealthCheckType& result) {
    if (value == "process") {
        result = HealthCheckType::kProcess;
        return true;
    }

    if (value == "http") {
        result = HealthCheckType::kHttp;
        return true;
    }

    if (value == "tcp") {
        result = HealthCheckType::kTcp;
        return true;
    }

    if (value == "command") {
        result = HealthCheckType::kCommand;
        return true;
    }

    return false;
}

[[nodiscard]] bool ParseHealthState(const std::string& value, HealthState& result) {
    if (value == "healthy") {
        result = HealthState::kHealthy;
        return true;
    }

    if (value == "unhealthy") {
        result = HealthState::kUnhealthy;
        return true;
    }

    if (value == "unknown") {
        result = HealthState::kUnknown;
        return true;
    }

    return false;
}

[[nodiscard]] bool ParseAlertMetric(const std::string& value, AlertMetric& result) {
    if (value == "cpu_percent") {
        result = AlertMetric::kCpuPercent;
        return true;
    }

    if (value == "rss_bytes") {
        result = AlertMetric::kRssBytes;
        return true;
    }

    if (value == "thread_count") {
        result = AlertMetric::kThreadCount;
        return true;
    }

    if (value == "fd_count") {
        result = AlertMetric::kFdCount;
        return true;
    }

    if (value == "health_state") {
        result = AlertMetric::kHealthState;
        return true;
    }

    return false;
}

[[nodiscard]] bool ParseAlertOperator(const std::string& value, AlertOperator& result) {
    if (value == ">") {
        result = AlertOperator::kGreaterThan;
        return true;
    }

    if (value == ">=") {
        result = AlertOperator::kGreaterThanOrEqual;
        return true;
    }

    if (value == "<") {
        result = AlertOperator::kLessThan;
        return true;
    }

    if (value == "<=") {
        result = AlertOperator::kLessThanOrEqual;
        return true;
    }

    if (value == "==" || value == "=") {
        result = AlertOperator::kEqual;
        return true;
    }

    if (value == "!=") {
        result = AlertOperator::kNotEqual;
        return true;
    }

    return false;
}

[[nodiscard]] bool ParseAlertSeverity(const std::string& value, AlertSeverity& result) {
    if (value == "info") {
        result = AlertSeverity::kInfo;
        return true;
    }

    if (value == "warning") {
        result = AlertSeverity::kWarning;
        return true;
    }

    if (value == "critical") {
        result = AlertSeverity::kCritical;
        return true;
    }

    return false;
}

[[nodiscard]] std::optional<HealthCheckDefinition>
ParseHealthCheckDefinition(const boost::property_tree::ptree& service_node, const std::string& service_id,
                           std::string& error) {
    HealthCheckDefinition definition;

    const auto health_node_optional = service_node.get_child_optional("health_check");

    if (!health_node_optional.has_value()) {
        return definition;
    }

    const boost::property_tree::ptree& health_node = *health_node_optional;

    definition.enabled = health_node.get<bool>("enabled", true);

    const std::string type_text = health_node.get<std::string>("type", "process");

    if (!ParseHealthCheckType(type_text, definition.type)) {
        error = "invalid health_check.type for service " + service_id + ": " + type_text;

        return std::nullopt;
    }

    definition.interval_seconds = health_node.get<std::uint32_t>("interval_seconds", 5);

    definition.timeout_milliseconds = health_node.get<std::uint32_t>("timeout_milliseconds", 1000);

    definition.failure_threshold = health_node.get<std::uint32_t>("failure_threshold", 3);

    definition.endpoint = health_node.get<std::string>("endpoint", "");

    definition.host = health_node.get<std::string>("host", "");

    const std::uint32_t port = health_node.get<std::uint32_t>("port", 0);

    if (port > 65535) {
        error = "health_check.port out of range for service " + service_id;

        return std::nullopt;
    }

    definition.port = static_cast<std::uint16_t>(port);

    definition.command = health_node.get<std::string>("command", "");

    if (!definition.IsStructurallyValid()) {
        error = "invalid health_check definition for service " + service_id;

        return std::nullopt;
    }

    return definition;
}

[[nodiscard]] std::optional<AlertRule> ParseAlertRule(const boost::property_tree::ptree& rule_node,
                                                      const std::string& service_id, std::string& error) {
    AlertRule rule;

    rule.service_id = service_id;

    const auto id_optional = rule_node.get_optional<std::string>("id");

    if (!id_optional.has_value()) {
        error = "alert rule missing id for service " + service_id;

        return std::nullopt;
    }

    rule.id = *id_optional;

    const auto metric_optional = rule_node.get_optional<std::string>("metric");

    if (!metric_optional.has_value()) {
        error = "alert rule " + rule.id + " missing metric for service " + service_id;

        return std::nullopt;
    }

    if (!ParseAlertMetric(*metric_optional, rule.metric)) {
        error = "invalid alert metric for service " + service_id + ", rule " + rule.id + ": " + *metric_optional;

        return std::nullopt;
    }

    const auto operator_optional = rule_node.get_optional<std::string>("operator");

    if (!operator_optional.has_value()) {
        error = "alert rule " + rule.id + " missing operator for service " + service_id;

        return std::nullopt;
    }

    if (!ParseAlertOperator(*operator_optional, rule.op)) {
        error = "invalid alert operator for service " + service_id + ", rule " + rule.id + ": " + *operator_optional;

        return std::nullopt;
    }

    rule.duration_seconds = rule_node.get<std::uint32_t>("duration_seconds", 0);

    const std::string severity_text = rule_node.get<std::string>("severity", "warning");

    if (!ParseAlertSeverity(severity_text, rule.severity)) {
        error = "invalid alert severity for service " + service_id + ", rule " + rule.id + ": " + severity_text;

        return std::nullopt;
    }

    rule.enabled = rule_node.get<bool>("enabled", true);

    rule.description = rule_node.get<std::string>("description", "");

    if (rule.metric == AlertMetric::kHealthState) {
        const auto threshold_optional = rule_node.get_optional<std::string>("threshold");

        if (!threshold_optional.has_value()) {
            error = "health_state alert rule " + rule.id + " missing threshold for service " + service_id;

            return std::nullopt;
        }

        HealthState health_state{HealthState::kUnknown};

        if (!ParseHealthState(*threshold_optional, health_state)) {
            error = "invalid health_state threshold for service " + service_id + ", rule " + rule.id + ": "
                    + *threshold_optional;

            return std::nullopt;
        }

        rule.health_state_threshold = health_state;
    } else {
        const auto threshold_optional = rule_node.get_optional<double>("threshold");

        if (!threshold_optional.has_value()) {
            error = "numeric alert rule " + rule.id + " missing numeric threshold for service " + service_id;

            return std::nullopt;
        }

        rule.numeric_threshold = *threshold_optional;
    }

    if (!rule.IsStructurallyValid()) {
        error = "invalid alert rule definition for service " + service_id + ", rule " + rule.id;

        return std::nullopt;
    }

    return rule;
}

[[nodiscard]] std::optional<std::vector<AlertRule>> ParseAlertRules(const boost::property_tree::ptree& service_node,
                                                                    const std::string& service_id, std::string& error) {
    std::vector<AlertRule> rules;

    const auto rules_node_optional = service_node.get_child_optional("alert_rules");

    if (!rules_node_optional.has_value()) {
        return rules;
    }

    std::unordered_set<std::string> rule_ids;

    for (const auto& item : *rules_node_optional) {
        const std::optional<AlertRule> rule = ParseAlertRule(item.second, service_id, error);

        if (!rule.has_value()) {
            return std::nullopt;
        }

        if (!rule_ids.insert(rule->id).second) {
            error = "duplicate alert rule id for service " + service_id + ": " + rule->id;

            return std::nullopt;
        }

        rules.push_back(*rule);
    }

    return rules;
}

[[nodiscard]] std::optional<RecoveryPolicy> ParseRecoveryPolicy(const boost::property_tree::ptree& service_node,
                                                                const std::string& service_id, std::string& error) {
    RecoveryPolicy policy;

    const auto policy_node_optional = service_node.get_child_optional("recovery_policy");

    if (!policy_node_optional.has_value()) {
        return policy;
    }

    const boost::property_tree::ptree& policy_node = *policy_node_optional;

    policy.enabled = policy_node.get<bool>("enabled", false);

    policy.restart_on_unhealthy = policy_node.get<bool>("restart_on_unhealthy", false);

    policy.restart_on_critical_alert = policy_node.get<bool>("restart_on_critical_alert", false);

    policy.max_restarts = policy_node.get<std::uint32_t>("max_restarts", 3);

    policy.window_seconds = policy_node.get<std::uint32_t>("window_seconds", 300);

    policy.backoff_seconds = policy_node.get<std::uint32_t>("backoff_seconds", 10);

    policy.startup_grace_seconds = policy_node.get<std::uint32_t>("startup_grace_seconds", 10);

    if (!policy.IsStructurallyValid()) {
        error = "invalid recovery_policy definition for service " + service_id;

        return std::nullopt;
    }

    return policy;
}

bool ParseServiceDefinition(const ptree& service_node, const std::size_t index,
                            const std::filesystem::path& path_base_dir, ServiceDefinition& definition,
                            std::string& error) {
    const std::string context = "services[" + std::to_string(index) + "]";

    if (!ReadRequiredString(service_node, "id", context, definition.id, error)) {
        return false;
    }

    if (!IsValidServiceId(definition.id)) {
        error = context + ".id is invalid: only letters, digits, '_' and '-' are allowed";
        return false;
    }

    if (!ReadRequiredString(service_node, "display_name", context, definition.display_name, error)) {
        return false;
    }

    std::string executable;
    std::string work_dir;
    std::string log_path;

    if (!ReadRequiredString(service_node, "executable", context, executable, error)
        || !ReadRequiredString(service_node, "work_dir", context, work_dir, error)
        || !ReadRequiredString(service_node, "log_path", context, log_path, error)) {
        return false;
    }

    if (!ReadArgs(service_node, context, definition.args, error)) {
        return false;
    }

    if (!ReadAutoStart(service_node, context, definition.auto_start, error)) {
        return false;
    }

    definition.executable = ResolvePath(path_base_dir, executable);
    definition.work_dir = ResolvePath(path_base_dir, work_dir);
    definition.log_path = ResolvePath(path_base_dir, log_path);

    const std::optional<HealthCheckDefinition> health_check =
        ParseHealthCheckDefinition(service_node, definition.id, error);

    if (!health_check.has_value()) {
        return false;
    }

    definition.health_check = *health_check;

    const std::optional<std::vector<AlertRule>> alert_rules = ParseAlertRules(service_node, definition.id, error);

    if (!alert_rules.has_value()) {
        return false;
    }

    definition.alert_rules = *alert_rules;

    const std::optional<RecoveryPolicy> recovery_policy = ParseRecoveryPolicy(service_node, definition.id, error);

    if (!recovery_policy.has_value()) {
        return false;
    }

    definition.recovery_policy = *recovery_policy;

    if (!definition.IsStructurallyValid()) {
        error = context + " is not structurally valid";
        return false;
    }

    return true;
}
} // namespace

bool ServiceRegistry::LoadFromFile(const std::filesystem::path& config_path, const std::filesystem::path& path_base_dir,
                                   std::string& error) {
    error.clear();

    if (!supervisors_.empty()) {
        error = "ServiceRegistry is already loaded; "
                "runtime config reload is not implemented yet";
        return false;
    }

    ptree root;

    try {
        boost::property_tree::read_json(config_path.string(), root);
    } catch (const boost::property_tree::ptree_error& exception) {
        error = "failed to parse config file " + config_path.string() + ": " + exception.what();
        return false;
    }

    const auto schema_version = root.get_optional<int>("schema_version");
    if (!schema_version.has_value()) {
        error = "schema_version is required";
        return false;
    }

    if (*schema_version != kSupportedSchemaVersion) {
        error = "unsupported schema_version: " + std::to_string(*schema_version);
        return false;
    }

    const auto services_node = root.get_child_optional("services");
    if (!services_node.has_value()) {
        error = "services array is required";
        return false;
    }

    if (services_node->empty()) {
        error = "services array must not be empty";
        return false;
    }

    std::unordered_map<std::string, std::unique_ptr<ProcessSupervisor>> parsed_supervisors;
    std::vector<std::string> parsed_order;

    std::size_t index = 0;

    for (const auto& [key, service_node] : *services_node) {
        if (!key.empty()) {
            error = "services must be a JSON array";
            return false;
        }

        ServiceDefinition definition;

        if (!ParseServiceDefinition(service_node, index, path_base_dir, definition, error)) {
            return false;
        }

        const std::string service_id = definition.id;

        if (parsed_supervisors.contains(service_id)) {
            error = "duplicate service id: " + service_id;
            return false;
        }

        parsed_supervisors.emplace(service_id, std::make_unique<ProcessSupervisor>(std::move(definition)));

        parsed_order.push_back(service_id);
        ++index;
    }
    supervisors_ = std::move(parsed_supervisors);
    service_order_ = std::move(parsed_order);

    return true;
}
ProcessSupervisor* ServiceRegistry::Find(const std::string_view service_id) noexcept {
    const auto iterator = supervisors_.find(std::string(service_id));

    if (iterator == supervisors_.end()) {
        return nullptr;
    }

    return iterator->second.get();
}
const ProcessSupervisor* ServiceRegistry::Find(const std::string_view service_id) const noexcept {
    const auto iterator = supervisors_.find(std::string(service_id));

    if (iterator == supervisors_.end()) {
        return nullptr;
    }

    return iterator->second.get();
}
std::vector<ServiceSummary> ServiceRegistry::ListServices() {
    std::vector<ServiceSummary> services;

    services.reserve(service_order_.size());

    for (const std::string& service_id : service_order_) {
        const auto iterator = supervisors_.find(service_id);

        if (iterator == supervisors_.end()) {
            continue;
        }

        ProcessSupervisor& supervisor = *iterator->second;

        const ServiceDefinition& definition = supervisor.Definition();

        services.push_back({
            .id = definition.id,
            .display_name = definition.display_name,
            .auto_start = definition.auto_start,
            .status = supervisor.GetStatus(),
        });
    }

    return services;
}
bool ServiceRegistry::StartAutoStartServices(std::string& error) {
    error.clear();

    std::vector<std::string> failures;

    for (const std::string& service_id : service_order_) {
        const auto iterator = supervisors_.find(service_id);

        if (iterator == supervisors_.end()) {
            continue;
        }

        ProcessSupervisor& supervisor = *iterator->second;

        if (!supervisor.Definition().auto_start) {
            continue;
        }

        std::string start_error;

        if (!supervisor.Start(start_error)) {
            failures.push_back(service_id + ": " + start_error);
        }
    }

    if (failures.empty()) {
        return true;
    }

    error = "auto-start failed: " + Join(failures, "; ");
    return false;
}
bool ServiceRegistry::StopAll(std::string& error) {
    error.clear();

    std::vector<std::string> failures;

    for (auto iterator = service_order_.rbegin(); iterator != service_order_.rend(); ++iterator) {
        const auto supervisor_iterator = supervisors_.find(*iterator);

        if (supervisor_iterator == supervisors_.end()) {
            continue;
        }

        std::string stop_error;

        if (!supervisor_iterator->second->Stop(stop_error)) {
            failures.push_back(*iterator + ": " + stop_error);
        }
    }

    if (failures.empty()) {
        return true;
    }

    error = "stop failed: " + Join(failures, "; ");
    return false;
}
std::size_t ServiceRegistry::Size() const noexcept {
    return supervisors_.size();
}
} // namespace aegis::agent