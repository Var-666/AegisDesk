#include "support/test_builders.h"

#include "agent/service/process_supervisor.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aegis::test {
namespace {

template <typename Predicate>
bool WaitUntil(Predicate predicate, const std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return predicate();
}

} // namespace

TEST(ProcessSupervisorStateTest, NewSupervisorStartsStopped) {
    ScopedTempDirectory directory("supervisor-initial");
    agent::ProcessSupervisor supervisor(ServiceDefinitionBuilder(directory.Path()).Build());

    const agent::ServiceStatus status = supervisor.GetStatus();

    EXPECT_EQ(status.state, agent::ServiceState::kStopped);
    EXPECT_EQ(status.desired_state, agent::DesiredState::kStopped);
    EXPECT_EQ(status.pid, -1);
    EXPECT_EQ(status.last_exit_kind, agent::ProcessExitKind::kNone);
    EXPECT_TRUE(status.last_error.empty());
    EXPECT_GT(status.last_transition_at_unix_ms, 0);
}

TEST(ProcessSupervisorStateTest, InvalidExecutableTransitionsToFailed) {
    ScopedTempDirectory directory("supervisor-failed");
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path()).WithExecutable(directory.Path() / "missing-service").Build();
    agent::ProcessSupervisor supervisor(definition);
    std::string error;

    ASSERT_FALSE(supervisor.Start(error));

    const agent::ServiceStatus status = supervisor.GetStatus();
    EXPECT_EQ(status.state, agent::ServiceState::kFailed);
    EXPECT_EQ(status.desired_state, agent::DesiredState::kRunning);
    EXPECT_EQ(status.pid, -1);
    EXPECT_EQ(status.last_error, error);
    EXPECT_NE(error.find("does not exist"), std::string::npos);
}

TEST(ProcessSupervisorStateTest, StartAndStopFollowRunningLifecycle) {
    ScopedTempDirectory directory("supervisor-running");
    const std::filesystem::path ready_file = directory.Path() / "ready";
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path()).WithArgs({"--ready-file", ready_file.string()}).Build();
    agent::ProcessSupervisor supervisor(definition);
    std::string error;

    ASSERT_TRUE(supervisor.Start(error)) << error;
    ASSERT_TRUE(WaitUntil([&] { return std::filesystem::exists(ready_file); }));

    const agent::ServiceStatus running = supervisor.GetStatus();
    EXPECT_EQ(running.state, agent::ServiceState::kRunning);
    EXPECT_EQ(running.desired_state, agent::DesiredState::kRunning);
    EXPECT_GT(running.pid, 0);
    EXPECT_TRUE(running.last_error.empty());

    ASSERT_TRUE(supervisor.Stop(error)) << error;

    const agent::ServiceStatus stopped = supervisor.GetStatus();
    EXPECT_EQ(stopped.state, agent::ServiceState::kStopped);
    EXPECT_EQ(stopped.desired_state, agent::DesiredState::kStopped);
    EXPECT_EQ(stopped.pid, -1);
    EXPECT_EQ(stopped.last_exit_kind, agent::ProcessExitKind::kExited);
    ASSERT_TRUE(stopped.exit_code.has_value());
    EXPECT_EQ(*stopped.exit_code, 0);
    EXPECT_GE(stopped.last_transition_at_unix_ms, running.last_transition_at_unix_ms);
}

TEST(ProcessSupervisorStateTest, NaturalExitTransitionsToExitedAndPreservesDesiredState) {
    ScopedTempDirectory directory("supervisor-exited");
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path()).WithArgs({"--exit-after-ms", "30", "--exit-code", "23"}).Build();
    agent::ProcessSupervisor supervisor(definition);
    std::string error;

    ASSERT_TRUE(supervisor.Start(error)) << error;

    agent::ServiceStatus final_status;
    ASSERT_TRUE(WaitUntil([&] {
        final_status = supervisor.GetStatus();
        return final_status.state == agent::ServiceState::kExited;
    }));

    EXPECT_EQ(final_status.desired_state, agent::DesiredState::kRunning);
    EXPECT_EQ(final_status.pid, -1);
    EXPECT_EQ(final_status.last_exit_kind, agent::ProcessExitKind::kExited);
    ASSERT_TRUE(final_status.exit_code.has_value());
    EXPECT_EQ(*final_status.exit_code, 23);
    EXPECT_FALSE(final_status.last_exit_signal.has_value());
    EXPECT_TRUE(final_status.last_error.empty());
}

TEST(ProcessSupervisorStateTest, StateAndExitKindsHaveStableStrings) {
    EXPECT_EQ(agent::ToString(agent::ServiceState::kStopped), "Stopped");
    EXPECT_EQ(agent::ToString(agent::ServiceState::kStarting), "Starting");
    EXPECT_EQ(agent::ToString(agent::ServiceState::kRunning), "Running");
    EXPECT_EQ(agent::ToString(agent::ServiceState::kStopping), "Stopping");
    EXPECT_EQ(agent::ToString(agent::ServiceState::kExited), "Exited");
    EXPECT_EQ(agent::ToString(agent::ServiceState::kFailed), "Failed");
    EXPECT_EQ(agent::ToString(agent::ProcessExitKind::kNone), "none");
    EXPECT_EQ(agent::ToString(agent::ProcessExitKind::kExited), "exited");
    EXPECT_EQ(agent::ToString(agent::ProcessExitKind::kSignaled), "signaled");
    EXPECT_EQ(agent::ToString(agent::ProcessExitKind::kUnknown), "unknown");
}

TEST(ProcessSupervisorConcurrencyTest, ConcurrentStartsCreateOnlyOneProcess) {
    ScopedTempDirectory directory("supervisor-concurrent-start");
    const std::filesystem::path ready_file = directory.Path() / "ready";
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path()).WithArgs({"--ready-file", ready_file.string()}).Build();
    agent::ProcessSupervisor supervisor(definition);

    constexpr int kCallerCount = 8;
    std::barrier start_gate(kCallerCount);
    std::atomic_int successful_starts{0};
    std::vector<std::thread> callers;
    callers.reserve(kCallerCount);

    for (int index = 0; index < kCallerCount; ++index) {
        callers.emplace_back([&] {
            start_gate.arrive_and_wait();

            std::string error;
            if (supervisor.Start(error)) {
                successful_starts.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::thread& caller : callers) {
        caller.join();
    }

    EXPECT_EQ(successful_starts.load(std::memory_order_relaxed), 1);
    ASSERT_TRUE(WaitUntil([&] { return std::filesystem::exists(ready_file); }));

    const agent::ServiceStatus status = supervisor.GetStatus();
    EXPECT_EQ(status.state, agent::ServiceState::kRunning);
    EXPECT_GT(status.pid, 0);

    std::string error;
    EXPECT_TRUE(supervisor.Stop(error)) << error;
}

TEST(ProcessSupervisorConcurrencyTest, StatusRemainsResponsiveWhileStopWaits) {
    ScopedTempDirectory directory("supervisor-responsive-stop");
    const std::filesystem::path ready_file = directory.Path() / "ready";
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path())
            .WithArgs({"--ready-file", ready_file.string(), "--ignore-sigterm", "--exit-after-ms", "400"})
            .Build();
    agent::ProcessSupervisor supervisor(definition);
    std::string start_error;

    ASSERT_TRUE(supervisor.Start(start_error)) << start_error;
    ASSERT_TRUE(WaitUntil([&] { return std::filesystem::exists(ready_file); }));

    auto stop_future = std::async(std::launch::async, [&] {
        std::string error;
        const bool succeeded = supervisor.Stop(error);
        return std::pair{succeeded, error};
    });

    agent::ServiceStatus stopping;
    ASSERT_TRUE(WaitUntil([&] {
        stopping = supervisor.GetStatus();
        return stopping.state == agent::ServiceState::kStopping;
    }));

    const auto query_started_at = std::chrono::steady_clock::now();
    const agent::ServiceStatus queried = supervisor.GetStatus();
    const auto query_duration = std::chrono::steady_clock::now() - query_started_at;

    EXPECT_EQ(queried.state, agent::ServiceState::kStopping);
    EXPECT_EQ(queried.desired_state, agent::DesiredState::kStopped);
    EXPECT_LT(query_duration, std::chrono::milliseconds(100));

    const auto [stopped, stop_error] = stop_future.get();
    EXPECT_TRUE(stopped) << stop_error;
    EXPECT_EQ(supervisor.GetStatus().state, agent::ServiceState::kStopped);
}

TEST(ProcessSupervisorConcurrencyTest, RestartKeepsDesiredStateRunningWhileStopping) {
    ScopedTempDirectory directory("supervisor-restart-desired");
    const std::filesystem::path ready_file = directory.Path() / "ready";
    const agent::ServiceDefinition definition =
        ServiceDefinitionBuilder(directory.Path())
            .WithArgs({"--ready-file", ready_file.string(), "--ignore-sigterm", "--exit-after-ms", "400"})
            .Build();
    agent::ProcessSupervisor supervisor(definition);
    std::string start_error;

    ASSERT_TRUE(supervisor.Start(start_error)) << start_error;
    ASSERT_TRUE(WaitUntil([&] { return std::filesystem::exists(ready_file); }));

    auto restart_future = std::async(std::launch::async, [&] {
        std::string error;
        const bool succeeded = supervisor.Restart(error);
        return std::pair{succeeded, error};
    });

    agent::ServiceStatus stopping;
    ASSERT_TRUE(WaitUntil([&] {
        stopping = supervisor.GetStatus();
        return stopping.state == agent::ServiceState::kStopping;
    }));
    EXPECT_EQ(stopping.desired_state, agent::DesiredState::kRunning);

    const auto [restarted, restart_error] = restart_future.get();
    ASSERT_TRUE(restarted) << restart_error;

    const agent::ServiceStatus running = supervisor.GetStatus();
    EXPECT_EQ(running.state, agent::ServiceState::kRunning);
    EXPECT_EQ(running.desired_state, agent::DesiredState::kRunning);

    std::string stop_error;
    EXPECT_TRUE(supervisor.Stop(stop_error)) << stop_error;
}

} // namespace aegis::test
