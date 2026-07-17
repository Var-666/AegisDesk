#pragma once

#include "agent/alerts/alert_rule.h"
#include "agent/health/health_status.h"
#include "agent/metrics/service_metrics.h"
#include "agent/service/process_supervisor.h"
#include "agent/service/service_definition.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace aegis::test {

class ScopedTempDirectory {
public:
    explicit ScopedTempDirectory(const std::string_view label = "case") {
        static std::atomic_uint64_t sequence{0};

        const std::string directory_name = "aegisdesk-" + std::string(label) + "-" + std::to_string(::getpid()) + "-"
                                           + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));

        path_ = std::filesystem::temp_directory_path() / directory_name;

        std::error_code error;
        std::filesystem::create_directories(path_, error);

        if (error) {
            throw std::runtime_error("failed to create test directory: " + error.message());
        }
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ServiceDefinitionBuilder {
public:
    explicit ServiceDefinitionBuilder(const std::filesystem::path& root) {
        definition_.id = "test_service";
        definition_.display_name = "Test Service";
        definition_.executable = FaultProcessPath();
        definition_.work_dir = root;
        definition_.log_path = root / "test_service.log";
        definition_.health_check.enabled = true;
        definition_.health_check.type = agent::HealthCheckType::kProcess;
        definition_.health_check.interval_seconds = 5;
        definition_.health_check.timeout_milliseconds = 1000;
        definition_.health_check.failure_threshold = 3;
    }

    ServiceDefinitionBuilder& WithId(std::string id) {
        definition_.id = std::move(id);
        return *this;
    }

    ServiceDefinitionBuilder& WithFailureThreshold(const std::uint32_t threshold) {
        definition_.health_check.failure_threshold = threshold;
        return *this;
    }

    ServiceDefinitionBuilder& WithHealthCheckEnabled(const bool enabled) {
        definition_.health_check.enabled = enabled;
        return *this;
    }

    ServiceDefinitionBuilder& WithAutoStart(const bool auto_start) {
        definition_.auto_start = auto_start;
        return *this;
    }

    ServiceDefinitionBuilder& WithRecoveryPolicy(agent::RecoveryPolicy policy) {
        definition_.recovery_policy = std::move(policy);
        return *this;
    }

    ServiceDefinitionBuilder& WithExecutable(std::filesystem::path executable) {
        definition_.executable = std::move(executable);
        return *this;
    }

    ServiceDefinitionBuilder& WithArgs(std::vector<std::string> args) {
        definition_.args = std::move(args);
        return *this;
    }

    [[nodiscard]] agent::ServiceDefinition Build() const {
        return definition_;
    }

    [[nodiscard]] static std::filesystem::path FaultProcessPath() {
        return std::filesystem::path(AEGIS_TEST_FAULT_PROCESS_PATH);
    }

    [[nodiscard]] static std::filesystem::path DemoServicePath() {
        return std::filesystem::path(AEGIS_TEST_DEMO_SERVICE_PATH);
    }

private:
    agent::ServiceDefinition definition_;
};

[[nodiscard]] inline agent::ServiceStatus MakeRunningStatus(const pid_t pid = 1234) {
    return agent::ServiceStatus{
        .state = agent::ServiceState::kRunning,
        .desired_state = agent::DesiredState::kRunning,
        .pid = pid,
        .exit_code = std::nullopt,
        .uptime = std::chrono::seconds(1),
    };
}

[[nodiscard]] inline agent::ServiceStatus MakeStoppedStatus() {
    return agent::ServiceStatus{
        .state = agent::ServiceState::kStopped,
        .desired_state = agent::DesiredState::kStopped,
        .pid = -1,
        .exit_code = std::nullopt,
        .uptime = std::chrono::seconds(0),
    };
}

[[nodiscard]] inline agent::ServiceMetrics MakeAvailableMetrics(const std::string& service_id = "test_service",
                                                                const double cpu_percent = 10.0) {
    return agent::ServiceMetrics{
        .service_id = service_id,
        .available = true,
        .pid = 1234,
        .collected_at_unix_ms = 1000,
        .cpu_percent = cpu_percent,
        .rss_bytes = 1024,
        .thread_count = 1,
        .fd_count = 4,
    };
}

[[nodiscard]] inline agent::HealthStatus MakeHealthStatus(const agent::HealthState state = agent::HealthState::kHealthy,
                                                          const std::string& service_id = "test_service",
                                                          const agent::UnixTimeMilliseconds checked_at = 1000) {
    return agent::HealthStatus{
        .service_id = service_id,
        .state = state,
        .reason = state == agent::HealthState::kUnhealthy ? "test failure" : "test status",
        .consecutive_failures = state == agent::HealthState::kUnhealthy ? 1U : 0U,
        .checked_at_unix_ms = checked_at,
    };
}

[[nodiscard]] inline agent::AlertRule MakeCpuAlertRule(const std::string& service_id = "test_service",
                                                       const double threshold = 80.0,
                                                       const std::uint32_t duration_seconds = 0) {
    return agent::AlertRule{
        .id = "high_cpu",
        .service_id = service_id,
        .metric = agent::AlertMetric::kCpuPercent,
        .op = agent::AlertOperator::kGreaterThan,
        .numeric_threshold = threshold,
        .health_state_threshold = std::nullopt,
        .duration_seconds = duration_seconds,
        .severity = agent::AlertSeverity::kWarning,
        .enabled = true,
        .description = "CPU is above the test threshold",
    };
}

} // namespace aegis::test
