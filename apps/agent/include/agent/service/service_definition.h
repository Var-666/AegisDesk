#pragma once

#include "agent/alerts/alert_rule.h"
#include "agent/health/health_status.h"
#include "agent/recovery/recovery_policy.h"
#include "agent/service/service_id.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace aegis::agent {

struct ServiceDefinition {
    std::string id;
    std::string display_name;

    std::filesystem::path executable;
    std::filesystem::path work_dir;

    std::vector<std::string> args;

    std::filesystem::path log_path;

    bool auto_start{false};

    HealthCheckDefinition health_check;

    std::vector<AlertRule> alert_rules;

    RecoveryPolicy recovery_policy;

    [[nodiscard]] bool IsStructurallyValid() const noexcept {
        if (!IsValidServiceId(id)) {
            return false;
        }

        if (display_name.empty()) {
            return false;
        }

        if (executable.empty()) {
            return false;
        }

        if (work_dir.empty()) {
            return false;
        }

        if (log_path.empty()) {
            return false;
        }

        if (!health_check.IsStructurallyValid()) {
            return false;
        }

        if (!recovery_policy.IsStructurallyValid()) {
            return false;
        }

        std::unordered_set<std::string> rule_ids;

        for (const AlertRule& rule : alert_rules) {
            if (!rule.IsStructurallyValid()) {
                return false;
            }

            if (rule.service_id != id) {
                return false;
            }

            if (!rule_ids.insert(rule.id).second) {
                return false;
            }
        }

        return true;
    }
};

} // namespace aegis::agent