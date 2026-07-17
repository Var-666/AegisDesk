#include "support/test_builders.h"

#include "agent/recovery/recovery_manager.h"
#include "agent/service/process_supervisor.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace aegis::test {
namespace {

template <typename Predicate>
bool WaitUntil(Predicate predicate, const std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return predicate();
}

agent::RecoveryPolicy MakeRecoveryPolicy(const std::uint32_t max_restarts = 3,
                                         const std::uint32_t backoff_seconds = 0) {
    return agent::RecoveryPolicy{
        .enabled = true,
        .restart_on_unhealthy = true,
        .restart_on_critical_alert = false,
        .max_restarts = max_restarts,
        .window_seconds = 100,
        .backoff_seconds = backoff_seconds,
        .startup_grace_seconds = 0,
    };
}

agent::HealthStatus MakeHealth(const agent::HealthState state, const agent::UnixTimeMilliseconds checked_at_unix_ms) {
    return agent::HealthStatus{
        .service_id = "test_service",
        .state = state,
        .reason = state == agent::HealthState::kUnhealthy ? "process is not running" : "process is running",
        .consecutive_failures = state == agent::HealthState::kUnhealthy ? 1U : 0U,
        .checked_at_unix_ms = checked_at_unix_ms,
    };
}

bool ContainsEvent(const agent::RecoveryManagerUpdate& update, const agent::RecoveryEventType type) {
    for (const agent::RecoveryEvent& event : update.events) {
        if (event.type == type) {
            return true;
        }
    }
    return false;
}

std::optional<pid_t> ReadSpawnedChildPid(const std::filesystem::path& log_path) {
    std::ifstream input(log_path);
    std::string line;
    constexpr std::string_view marker = "child spawned pid=";

    while (std::getline(input, line)) {
        const std::size_t position = line.find(marker);
        if (position == std::string::npos) {
            continue;
        }

        try {
            return static_cast<pid_t>(std::stol(line.substr(position + marker.size())));
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

bool ProcessIsGone(const pid_t process_id) {
    if (process_id <= 0) {
        return true;
    }

    if (kill(process_id, 0) == 0) {
        return false;
    }

    return errno == ESRCH;
}

} // namespace

TEST(RecoveryManagerIntegrationTest, AbnormalExitIsAutomaticallyRestarted) {
    ScopedTempDirectory directory("recovery-abnormal-exit");
    const agent::ServiceDefinition definition = ServiceDefinitionBuilder(directory.Path())
                                                    .WithExecutable(ServiceDefinitionBuilder::DemoServicePath())
                                                    .WithArgs({"--name", "auto_recovery_demo", "--interval-ms", "100",
                                                               "--exit-after", "1", "--exit-code", "23"})
                                                    .WithRecoveryPolicy(MakeRecoveryPolicy())
                                                    .Build();
    agent::ProcessSupervisor supervisor(definition);
    agent::RecoveryManager recovery_manager;
    std::string error;

    ASSERT_TRUE(supervisor.Start(error)) << error;
    const pid_t first_pid = supervisor.GetStatus().pid;
    ASSERT_GT(first_pid, 0);

    recovery_manager.EvaluateAndRecover(supervisor, MakeHealth(agent::HealthState::kHealthy, 1000), {}, 1000);
    ASSERT_TRUE(WaitUntil([&] { return supervisor.GetStatus().state == agent::ServiceState::kExited; }));

    const agent::RecoveryManagerUpdate update =
        recovery_manager.EvaluateAndRecover(supervisor, MakeHealth(agent::HealthState::kUnhealthy, 2000), {}, 2000);

    EXPECT_TRUE(ContainsEvent(update, agent::RecoveryEventType::kRestartStarted));
    EXPECT_TRUE(ContainsEvent(update, agent::RecoveryEventType::kRestartSucceeded));

    const agent::ServiceStatus restarted = supervisor.GetStatus();
    EXPECT_EQ(restarted.desired_state, agent::DesiredState::kRunning);
    EXPECT_EQ(restarted.state, agent::ServiceState::kRunning);
    EXPECT_GT(restarted.pid, 0);
    EXPECT_NE(restarted.pid, first_pid);

    EXPECT_TRUE(supervisor.Stop(error)) << error;
}

TEST(RecoveryManagerIntegrationTest, ManualStopIsNeverAutomaticallyRestarted) {
    ScopedTempDirectory directory("recovery-manual-stop");
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path()).WithRecoveryPolicy(MakeRecoveryPolicy()).Build();
    agent::ProcessSupervisor supervisor(definition);
    agent::RecoveryManager recovery_manager;
    std::string error;

    ASSERT_TRUE(supervisor.Start(error)) << error;
    recovery_manager.EvaluateAndRecover(supervisor, MakeHealth(agent::HealthState::kHealthy, 1000), {}, 1000);
    ASSERT_TRUE(supervisor.Stop(error)) << error;

    const agent::RecoveryManagerUpdate update =
        recovery_manager.EvaluateAndRecover(supervisor, MakeHealth(agent::HealthState::kUnhealthy, 2000), {}, 2000);

    EXPECT_TRUE(update.events.empty());
    const agent::ServiceStatus status = supervisor.GetStatus();
    EXPECT_EQ(status.state, agent::ServiceState::kStopped);
    EXPECT_EQ(status.desired_state, agent::DesiredState::kStopped);
    EXPECT_EQ(status.pid, -1);
}

TEST(RecoveryManagerPolicyTest, BackoffAndRestartBudgetSuppressRepeatedFailures) {
    ScopedTempDirectory directory("recovery-policy");
    const agent::ServiceDefinition definition = ServiceDefinitionBuilder(directory.Path())
                                                    .WithExecutable(directory.Path() / "missing-service")
                                                    .WithAutoStart(true)
                                                    .WithRecoveryPolicy(MakeRecoveryPolicy(2, 10))
                                                    .Build();
    agent::ProcessSupervisor supervisor(definition);
    agent::RecoveryManager recovery_manager({
        .recent_event_capacity = 50,
        .suppress_event_cooldown_seconds = 1,
    });

    const agent::RecoveryManagerUpdate first =
        recovery_manager.EvaluateAndRecover(supervisor, MakeHealth(agent::HealthState::kUnhealthy, 10000), {}, 10000);
    EXPECT_TRUE(ContainsEvent(first, agent::RecoveryEventType::kRestartFailed));

    const agent::RecoveryManagerUpdate backoff =
        recovery_manager.EvaluateAndRecover(supervisor, MakeHealth(agent::HealthState::kUnhealthy, 11000), {}, 11000);
    ASSERT_EQ(backoff.events.size(), 1U);
    EXPECT_EQ(backoff.events[0].type, agent::RecoveryEventType::kRestartSuppressed);
    EXPECT_NE(backoff.events[0].reason.find("backoff"), std::string::npos);

    const agent::RecoveryManagerUpdate second =
        recovery_manager.EvaluateAndRecover(supervisor, MakeHealth(agent::HealthState::kUnhealthy, 20000), {}, 20000);
    EXPECT_TRUE(ContainsEvent(second, agent::RecoveryEventType::kRestartFailed));

    const agent::RecoveryManagerUpdate budget =
        recovery_manager.EvaluateAndRecover(supervisor, MakeHealth(agent::HealthState::kUnhealthy, 30000), {}, 30000);
    ASSERT_EQ(budget.events.size(), 1U);
    EXPECT_EQ(budget.events[0].type, agent::RecoveryEventType::kRestartSuppressed);
    EXPECT_NE(budget.events[0].reason.find("restart limit"), std::string::npos);
    EXPECT_EQ(budget.events[0].restart_count_in_window, 2U);
}

TEST(DemoServiceFaultInjectionTest, ExitAfterReturnsConfiguredFailureCode) {
    ScopedTempDirectory directory("demo-exit-after");
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path())
            .WithExecutable(ServiceDefinitionBuilder::DemoServicePath())
            .WithArgs({"--name", "exit_demo", "--interval-ms", "100", "--exit-after", "1", "--exit-code", "23",
                       "--cpu-load-percent", "10"})
            .Build();
    agent::ProcessSupervisor supervisor(definition);
    std::string error;

    ASSERT_TRUE(supervisor.Start(error)) << error;
    ASSERT_TRUE(WaitUntil([&] { return supervisor.GetStatus().state == agent::ServiceState::kExited; }));

    const agent::ServiceStatus exited = supervisor.GetStatus();
    EXPECT_EQ(exited.last_exit_kind, agent::ProcessExitKind::kExited);
    ASSERT_TRUE(exited.exit_code.has_value());
    EXPECT_EQ(*exited.exit_code, 23);
}

TEST(DemoServiceFaultInjectionTest, IgnoredSigtermEscalatesAndLeavesNoChildProcess) {
    ScopedTempDirectory directory("demo-process-tree");
    const std::filesystem::path demo_log = directory.Path() / "runtime/logs/process_tree_demo.log";
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path())
            .WithExecutable(ServiceDefinitionBuilder::DemoServicePath())
            .WithArgs({"--name", "process_tree_demo", "--interval-ms", "100", "--ignore-sigterm", "--spawn-child"})
            .Build();
    agent::ProcessSupervisor supervisor(definition);
    std::string error;

    ASSERT_TRUE(supervisor.Start(error)) << error;
    std::optional<pid_t> child_pid;
    ASSERT_TRUE(WaitUntil([&] {
        child_pid = ReadSpawnedChildPid(demo_log);
        return child_pid.has_value();
    }));

    ASSERT_TRUE(supervisor.Stop(error)) << error;
    ASSERT_TRUE(child_pid.has_value());
    EXPECT_TRUE(WaitUntil([&] { return ProcessIsGone(*child_pid); }));

    const agent::ServiceStatus stopped = supervisor.GetStatus();
    EXPECT_EQ(stopped.state, agent::ServiceState::kStopped);
    EXPECT_EQ(stopped.process_group_id, -1);
    EXPECT_EQ(stopped.last_exit_kind, agent::ProcessExitKind::kSignaled);
    ASSERT_TRUE(stopped.last_exit_signal.has_value());
    EXPECT_EQ(*stopped.last_exit_signal, SIGKILL);
}

} // namespace aegis::test
