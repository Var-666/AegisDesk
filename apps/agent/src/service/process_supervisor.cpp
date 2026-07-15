//
// Created by Var on 2026/7/3.
//

#include "agent/service/process_supervisor.h"

#include <csignal>
#include <iostream>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace aegis::agent {
namespace {

constexpr auto kGracefulStopTimeout = std::chrono::seconds(3);
constexpr auto kForcedStopTimeout = std::chrono::seconds(3);
constexpr auto kExitPollInterval = std::chrono::milliseconds(25);

} // namespace

std::string ToString(const ServiceState state) {
    switch (state) {
        case ServiceState::kStopped:
            return "Stopped";
        case ServiceState::kStarting:
            return "Starting";
        case ServiceState::kRunning:
            return "Running";
        case ServiceState::kStopping:
            return "Stopping";
        case ServiceState::kExited:
            return "Exited";
        case ServiceState::kFailed:
            return "Failed";
    }
    return "Unknown";
}

std::string ToString(const ProcessExitKind kind) {
    switch (kind) {
        case ProcessExitKind::kNone:
            return "none";
        case ProcessExitKind::kExited:
            return "exited";
        case ProcessExitKind::kSignaled:
            return "signaled";
        case ProcessExitKind::kUnknown:
            return "unknown";
    }
    return "unknown";
}

ProcessSupervisor::ProcessSupervisor(ServiceDefinition definition)
    : definition_(std::move(definition))
    , desired_state_(definition_.auto_start ? DesiredState::kRunning : DesiredState::kStopped)
    , last_transition_at_unix_ms_(NowUnixTimeMilliseconds()) {
    if (definition_.IsStructurallyValid()) {
        definition_.executable = std::filesystem::absolute(definition_.executable);
        definition_.work_dir = std::filesystem::absolute(definition_.work_dir);
        definition_.log_path = std::filesystem::absolute(definition_.log_path);
    }
}
ProcessSupervisor::~ProcessSupervisor() noexcept {
    std::string error;

    if (!Stop(error)) {
        std::cerr << "[agent] destructor stop failed: " << error << '\n';
    }
}
bool ProcessSupervisor::Start(std::string& error) {
    std::scoped_lock operation_lock(operation_mutex_);
    return StartOperation(error);
}
bool ProcessSupervisor::Stop(std::string& error) {
    std::scoped_lock operation_lock(operation_mutex_);
    return StopOperation(error, DesiredState::kStopped);
}
bool ProcessSupervisor::Restart(std::string& error) {
    std::scoped_lock operation_lock(operation_mutex_);

    {
        std::scoped_lock state_lock(state_mutex_);
        desired_state_ = DesiredState::kRunning;
    }

    if (!StopOperation(error, DesiredState::kRunning)) {
        return false;
    }

    return StartOperation(error);
}
ServiceStatus ProcessSupervisor::GetStatus() {
    std::scoped_lock state_lock(state_mutex_);

    // StopOperation is the sole reaper while a stop is in progress. This
    // prevents status polling from racing with the operation's waitpid calls.
    if (state_ != ServiceState::kStopping) {
        ReapExitedChildLocked();
    }

    ServiceStatus status;
    status.state = state_;
    status.desired_state = desired_state_;
    status.pid = pid_;
    status.exit_code = last_exit_code_;
    status.last_exit_kind = last_exit_kind_;
    status.last_exit_signal = last_exit_signal_;
    status.last_error = last_error_;
    status.last_transition_at_unix_ms = last_transition_at_unix_ms_;

    if (start_time_.has_value()) {
        status.uptime =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - *start_time_);
    }

    return status;
}
const ServiceDefinition& ProcessSupervisor::Definition() const noexcept {
    return definition_;
}
DesiredState ProcessSupervisor::GetDesiredState() const {
    std::scoped_lock state_lock(state_mutex_);
    return desired_state_;
}
void ProcessSupervisor::SetDesiredState(const DesiredState desired_state) {
    std::scoped_lock operation_lock(operation_mutex_);
    std::scoped_lock state_lock(state_mutex_);
    desired_state_ = desired_state;
}
bool ProcessSupervisor::StartOperation(std::string& error) {
    error.clear();

    {
        std::scoped_lock state_lock(state_mutex_);

        desired_state_ = DesiredState::kRunning;

        ReapExitedChildLocked();

        if (pid_ > 0) {
            error = definition_.id + " is already running";
            return false;
        }

        TransitionStateLocked(ServiceState::kStarting);
    }

    if (!definition_.IsStructurallyValid()) {
        error = "invalid service definition: " + definition_.id;

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    if (!std::filesystem::exists(definition_.executable)) {
        error = "service executable does not exist: " + definition_.executable.string();

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    if (!std::filesystem::exists(definition_.work_dir) || !std::filesystem::is_directory(definition_.work_dir)) {
        error = "service work directory does not exist or is not a directory: " + definition_.work_dir.string();

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    std::vector<std::string> argv_storage;
    argv_storage.reserve(1 + definition_.args.size());
    argv_storage.push_back(definition_.executable.string());

    for (const std::string& argument : definition_.args) {
        argv_storage.push_back(argument);
    }

    std::vector<char*> argv;

    argv.reserve(argv_storage.size() + 1);

    for (std::string& argument : argv_storage) {
        argv.push_back(argument.data());
    }

    argv.push_back(nullptr);

    const std::string executable_path = definition_.executable.string();
    const std::string work_dir = definition_.work_dir.string();

    const pid_t child_pid = fork();

    if (child_pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    if (child_pid == 0) {
        if (chdir(work_dir.c_str()) != 0) {
            std::cerr << "agent child: chdir failed: " << std::strerror(errno) << '\n';
            _exit(127);
        }

        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        execv(executable_path.c_str(), argv.data());

        _exit(127);
    }

    {
        std::scoped_lock state_lock(state_mutex_);

        pid_ = child_pid;
        start_time_ = std::chrono::steady_clock::now();
        TransitionStateLocked(ServiceState::kRunning);
    }

    return true;
}
bool ProcessSupervisor::StopOperation(std::string& error, const DesiredState final_desired_state) {
    error.clear();

    pid_t target_pid = -1;

    {
        std::scoped_lock state_lock(state_mutex_);

        desired_state_ = final_desired_state;

        ReapExitedChildLocked();

        if (pid_ <= 0) {
            if (final_desired_state == DesiredState::kStopped && state_ != ServiceState::kStopped) {
                TransitionStateLocked(ServiceState::kStopped);
            }
            return true;
        }

        target_pid = pid_;

        TransitionStateLocked(ServiceState::kStopping);
    }

    if (kill(target_pid, SIGTERM) != 0 && errno != ESRCH) {
        error = std::string("SIGTERM failed: ") + std::strerror(errno);

        std::scoped_lock state_lock(state_mutex_);
        if (pid_ == target_pid) {
            TransitionStateLocked(ServiceState::kFailed, error);
        }
        return false;
    }

    if (WaitForExit(target_pid, std::chrono::steady_clock::now() + kGracefulStopTimeout)) {
        return true;
    }

    if (kill(target_pid, SIGKILL) != 0 && errno != ESRCH) {
        error = std::string("SIGKILL failed: ") + std::strerror(errno);

        std::scoped_lock state_lock(state_mutex_);
        if (pid_ == target_pid) {
            TransitionStateLocked(ServiceState::kFailed, error);
        }
        return false;
    }

    if (WaitForExit(target_pid, std::chrono::steady_clock::now() + kForcedStopTimeout)) {
        return true;
    }

    error = "timed out waiting for service to exit after SIGKILL: " + definition_.id;

    {
        std::scoped_lock state_lock(state_mutex_);
        if (pid_ == target_pid) {
            TransitionStateLocked(ServiceState::kFailed, error);
        }
    }

    return false;
}
bool ProcessSupervisor::WaitForExit(const pid_t target_pid, const std::chrono::steady_clock::time_point deadline) {
    while (true) {
        {
            std::scoped_lock state_lock(state_mutex_);

            if (pid_ != target_pid) {
                return true;
            }

            if (ReapExitedChildLocked()) {
                return true;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        std::this_thread::sleep_for(kExitPollInterval);
    }
}
bool ProcessSupervisor::ReapExitedChildLocked() {
    if (pid_ <= 0) {
        return false;
    }

    int status = 0;
    const pid_t result = waitpid(pid_, &status, WNOHANG);

    if (result == 0) {
        return false;
    }

    if (result == pid_) {
        SaveExitStatusLocked(status);
        MarkChildExitedLocked();
        return true;
    }

    if (result < 0 && errno == ECHILD) {
        last_exit_code_.reset();
        last_exit_kind_ = ProcessExitKind::kUnknown;
        last_exit_signal_.reset();
        MarkChildExitedLocked();
        return true;
    }

    return false;
}
void ProcessSupervisor::SaveExitStatusLocked(int status) {
    if (WIFEXITED(status)) {
        last_exit_code_ = WEXITSTATUS(status);
        last_exit_kind_ = ProcessExitKind::kExited;
        last_exit_signal_.reset();
        return;
    }

    if (WIFSIGNALED(status)) {
        const int signal = WTERMSIG(status);
        last_exit_code_ = 128 + signal;
        last_exit_kind_ = ProcessExitKind::kSignaled;
        last_exit_signal_ = signal;
        return;
    }

    last_exit_code_.reset();
    last_exit_kind_ = ProcessExitKind::kUnknown;
    last_exit_signal_.reset();
}
void ProcessSupervisor::MarkChildExitedLocked() {
    pid_ = -1;
    start_time_.reset();

    const bool explicitly_stopped = desired_state_ == DesiredState::kStopped || state_ == ServiceState::kStopping;
    TransitionStateLocked(explicitly_stopped ? ServiceState::kStopped : ServiceState::kExited);
}
void ProcessSupervisor::TransitionStateLocked(const ServiceState state, std::string error) {
    state_ = state;
    last_error_ = std::move(error);
    last_transition_at_unix_ms_ = NowUnixTimeMilliseconds();
}
UnixTimeMilliseconds ProcessSupervisor::NowUnixTimeMilliseconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    return milliseconds < 0 ? 0 : static_cast<UnixTimeMilliseconds>(milliseconds);
}
} // namespace aegis::agent
