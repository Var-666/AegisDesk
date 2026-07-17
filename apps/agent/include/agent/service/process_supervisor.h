#pragma once

#include "agent/metrics/service_metrics.h"
#include "agent/service/desired_state.h"
#include "agent/service/service_definition.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <sys/types.h>

namespace aegis::agent {
enum class ServiceState {
    kStopped,
    kStarting,
    kRunning,
    kStopping,
    kExited,
    kFailed,
};

[[nodiscard]] std::string ToString(ServiceState state);

enum class ProcessExitKind {
    kNone,
    kExited,
    kSignaled,
    kUnknown,
};

[[nodiscard]] std::string ToString(ProcessExitKind kind);

struct ServiceStatus {
    ServiceState state{ServiceState::kStopped};
    DesiredState desired_state{DesiredState::kStopped};
    pid_t pid{-1};
    std::optional<int> exit_code;
    std::chrono::seconds uptime{0};
    ProcessExitKind last_exit_kind{ProcessExitKind::kNone};
    std::optional<int> last_exit_signal;
    std::string last_error;
    UnixTimeMilliseconds last_transition_at_unix_ms{0};
};

class ProcessSupervisor {
public:
    explicit ProcessSupervisor(ServiceDefinition definition);

    ~ProcessSupervisor() noexcept;

    ProcessSupervisor(const ProcessSupervisor&) = delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

    bool Start(std::string& error);
    bool Stop(std::string& error);
    bool Restart(std::string& error);

    [[nodiscard]] ServiceStatus GetStatus() const;

    [[nodiscard]] const ServiceDefinition& Definition() const noexcept;

    [[nodiscard]] DesiredState GetDesiredState() const;

    void SetDesiredState(DesiredState desired_state);

private:
    bool StartOperation(std::string& error);
    bool StopOperation(std::string& error, DesiredState final_desired_state);
    bool WaitForObservedExit(pid_t target_pid, std::chrono::steady_clock::time_point deadline);

    void ObserveChildExit(pid_t target_pid) noexcept;
    void JoinObserverThread() noexcept;
    void SaveExitStatusLocked(int status);
    void MarkChildExitedLocked();
    void TransitionStateLocked(ServiceState state, std::string error = {});

    [[nodiscard]] static UnixTimeMilliseconds NowUnixTimeMilliseconds() noexcept;

private:
    ServiceDefinition definition_;
    DesiredState desired_state_{DesiredState::kStopped};

    std::mutex operation_mutex_;
    mutable std::mutex state_mutex_;
    std::condition_variable exit_condition_;
    std::thread observer_thread_;

    ServiceState state_{ServiceState::kStopped};
    pid_t pid_{-1};
    std::optional<int> last_exit_code_;
    ProcessExitKind last_exit_kind_{ProcessExitKind::kNone};
    std::optional<int> last_exit_signal_;
    std::string last_error_;
    UnixTimeMilliseconds last_transition_at_unix_ms_{0};
    std::optional<std::chrono::steady_clock::time_point> start_time_;
};
} // namespace aegis::agent
