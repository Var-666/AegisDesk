#pragma once

#include "agent/alerts/alert_rule.h"
#include "agent/health/health_status.h"
#include "agent/recovery/recovery_policy.h"
#include "agent/service/process_supervisor.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aegis::agent {
struct ServiceSummary {
    std::string id;
    std::string display_name;
    bool auto_start{false};
    ServiceStatus status;
};

class ServiceRegistry {
public:
    ServiceRegistry() = default;

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    bool LoadFromFile(const std::filesystem::path& config_path, const std::filesystem::path& path_base_dir,
                      std::string& error);

    ProcessSupervisor* Find(std::string_view service_id) noexcept;
    const ProcessSupervisor* Find(std::string_view service_id) const noexcept;

    std::vector<ServiceSummary> ListServices();

    bool StartAutoStartServices(std::string& error);

    bool StopAll(std::string& error);

    [[nodiscard]] std::size_t Size() const noexcept;

private:
    std::unordered_map<std::string, std::unique_ptr<ProcessSupervisor>> supervisors_;
    std::vector<std::string> service_order_;
};
} // namespace aegis::agent
