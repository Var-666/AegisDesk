#include "agent/service/process_supervisor.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aegis::agent {
namespace {

constexpr auto kGracefulStopTimeout = std::chrono::seconds(3);
constexpr auto kForcedStopTimeout = std::chrono::seconds(3);
constexpr auto kExecConfirmationTimeout = std::chrono::seconds(5);
constexpr auto kProcessGroupPollInterval = std::chrono::milliseconds(25);

std::mutex g_fork_mutex;

enum class ChildStartStage : int {
    kCreateProcessGroup = 1,
    kChangeDirectory,
    kOpenNullDevice,
    kRedirectStandardOutput,
    kRedirectStandardError,
    kExecute,
};

bool ProcessGroupExists(const pid_t process_group_id) {
    if (process_group_id <= 0) {
        return false;
    }

    if (kill(-process_group_id, 0) == 0) {
        return true;
    }

    return errno == EPERM;
}

int SignalProcessTree(const pid_t process_group_id, const pid_t leader_pid, const int signal) {
    if (process_group_id > 0) {
        return kill(-process_group_id, signal);
    }

    return leader_pid > 0 ? kill(leader_pid, signal) : 0;
}

bool WaitForProcessGroupExit(const pid_t process_group_id, const std::chrono::steady_clock::time_point deadline) {
    while (ProcessGroupExists(process_group_id)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(kProcessGroupPollInterval);
    }

    return true;
}

struct ChildStartError {
    ChildStartStage stage;
    int error_number;
};

enum class ExecConfirmationResult {
    kSucceeded,
    kChildFailed,
    kTimedOut,
    kPipeFailed,
};

bool SetCloseOnExec(const int file_descriptor) {
    const int flags = fcntl(file_descriptor, F_GETFD);
    return flags >= 0 && fcntl(file_descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool MovePipeDescriptorsAboveStandardStreams(int (&error_pipe)[2]) {
    for (int& file_descriptor : error_pipe) {
        if (file_descriptor > STDERR_FILENO) {
            continue;
        }

        const int moved_descriptor = fcntl(file_descriptor, F_DUPFD, STDERR_FILENO + 1);
        if (moved_descriptor < 0) {
            return false;
        }

        close(file_descriptor);
        file_descriptor = moved_descriptor;
    }

    return true;
}

void ReportChildStartErrorAndExit(const int error_pipe, const ChildStartStage stage, const int error_number) {
    const ChildStartError message{
        .stage = stage,
        .error_number = error_number,
    };

    auto data = reinterpret_cast<const char*>(&message);
    std::size_t remaining = sizeof(message);

    while (remaining > 0) {
        const ssize_t written = write(error_pipe, data, remaining);

        if (written > 0) {
            data += written;
            remaining -= static_cast<std::size_t>(written);
            continue;
        }

        if (written < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    _exit(127);
}

ExecConfirmationResult WaitForExecConfirmation(const int error_pipe, ChildStartError& child_error,
                                               int& pipe_error_number) {
    const auto deadline = std::chrono::steady_clock::now() + kExecConfirmationTimeout;
    const auto destination = reinterpret_cast<char*>(&child_error);
    std::size_t bytes_read = 0;

    while (true) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            return ExecConfirmationResult::kTimedOut;
        }

        const auto remaining_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining + std::chrono::milliseconds(1));
        pollfd descriptor{
            .fd = error_pipe,
            .events = POLLIN,
            .revents = 0,
        };

        const int poll_result = poll(&descriptor, 1, static_cast<int>(remaining_milliseconds.count()));

        if (poll_result == 0) {
            return ExecConfirmationResult::kTimedOut;
        }

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            pipe_error_number = errno;
            return ExecConfirmationResult::kPipeFailed;
        }

        const ssize_t result = read(error_pipe, destination + bytes_read, sizeof(child_error) - bytes_read);

        if (result == 0) {
            if (bytes_read == 0) {
                return ExecConfirmationResult::kSucceeded;
            }

            pipe_error_number = EPROTO;
            return ExecConfirmationResult::kPipeFailed;
        }

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            pipe_error_number = errno;
            return ExecConfirmationResult::kPipeFailed;
        }

        bytes_read += static_cast<std::size_t>(result);
        if (bytes_read == sizeof(child_error)) {
            return ExecConfirmationResult::kChildFailed;
        }
    }
}

void ReapChildBlocking(const pid_t child_pid) {
    while (waitpid(child_pid, nullptr, 0) < 0 && errno == EINTR) {}
}

std::string DescribeChildStartError(const ChildStartError& child_error, const std::string& executable_path,
                                    const std::string& work_dir) {
    std::string operation;

    switch (child_error.stage) {
        case ChildStartStage::kCreateProcessGroup:
            operation = "setpgid(0, 0)";
            break;
        case ChildStartStage::kChangeDirectory:
            operation = "chdir('" + work_dir + "')";
            break;
        case ChildStartStage::kOpenNullDevice:
            operation = "open('/dev/null')";
            break;
        case ChildStartStage::kRedirectStandardOutput:
            operation = "redirect stdout";
            break;
        case ChildStartStage::kRedirectStandardError:
            operation = "redirect stderr";
            break;
        case ChildStartStage::kExecute:
            operation = "execv('" + executable_path + "')";
            break;
    }

    return "service startup failed during " + operation + ": " + std::strerror(child_error.error_number);
}

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

        pid_t target_pid = -1;
        pid_t target_process_group_id = -1;
        {
            std::scoped_lock state_lock(state_mutex_);
            target_pid = pid_;
            target_process_group_id = process_group_id_;
        }

        if (target_pid > 0 || target_process_group_id > 0) {
            SignalProcessTree(target_process_group_id, target_pid, SIGKILL);
        }
    }

    JoinObserverThread();
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
ServiceStatus ProcessSupervisor::GetStatus() const {
    std::scoped_lock state_lock(state_mutex_);

    ServiceStatus status;
    status.state = state_;
    status.desired_state = desired_state_;
    status.pid = pid_;
    status.process_group_id = process_group_id_;
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

        if (pid_ > 0) {
            error = definition_.id + " is already running";
            return false;
        }
    }

    JoinObserverThread();

    {
        std::scoped_lock state_lock(state_mutex_);
        if (process_group_id_ > 0) {
            error = definition_.id + " still has an active process group";
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
    char* const* const argv_data = argv.data();
    const char* const executable_path_data = executable_path.c_str();
    const char* const work_dir_data = work_dir.c_str();

    int error_pipe[2] = {-1, -1};
    std::unique_lock fork_lock(g_fork_mutex);

    if (pipe(error_pipe) != 0) {
        error = std::string("failed to create exec confirmation pipe: ") + std::strerror(errno);

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    if (!MovePipeDescriptorsAboveStandardStreams(error_pipe)) {
        const int error_number = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        error = std::string("failed to configure exec confirmation pipe descriptors: ") + std::strerror(error_number);

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    if (!SetCloseOnExec(error_pipe[1])) {
        const int error_number = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        error = std::string("failed to configure exec confirmation pipe: ") + std::strerror(error_number);

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    const pid_t child_pid = fork();

    if (child_pid < 0) {
        const int error_number = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        error = std::string("fork failed: ") + std::strerror(error_number);

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    if (child_pid == 0) {
        close(error_pipe[0]);

        if (setpgid(0, 0) != 0) {
            ReportChildStartErrorAndExit(error_pipe[1], ChildStartStage::kCreateProcessGroup, errno);
        }

        if (chdir(work_dir_data) != 0) {
            ReportChildStartErrorAndExit(error_pipe[1], ChildStartStage::kChangeDirectory, errno);
        }

        const int null_device = open("/dev/null", O_WRONLY);
        if (null_device < 0) {
            ReportChildStartErrorAndExit(error_pipe[1], ChildStartStage::kOpenNullDevice, errno);
        }

        if (dup2(null_device, STDOUT_FILENO) < 0) {
            ReportChildStartErrorAndExit(error_pipe[1], ChildStartStage::kRedirectStandardOutput, errno);
        }

        if (dup2(null_device, STDERR_FILENO) < 0) {
            ReportChildStartErrorAndExit(error_pipe[1], ChildStartStage::kRedirectStandardError, errno);
        }

        if (null_device != STDOUT_FILENO && null_device != STDERR_FILENO) {
            close(null_device);
        }

        execv(executable_path_data, argv_data);
        ReportChildStartErrorAndExit(error_pipe[1], ChildStartStage::kExecute, errno);
    }

    close(error_pipe[1]);
    fork_lock.unlock();

    ChildStartError child_error{};
    int pipe_error_number = 0;
    const ExecConfirmationResult confirmation = WaitForExecConfirmation(error_pipe[0], child_error, pipe_error_number);
    close(error_pipe[0]);

    if (confirmation != ExecConfirmationResult::kSucceeded) {
        if (confirmation == ExecConfirmationResult::kChildFailed) {
            error = DescribeChildStartError(child_error, executable_path, work_dir);
        } else if (confirmation == ExecConfirmationResult::kTimedOut) {
            error = "timed out waiting for exec confirmation: " + definition_.id;
            kill(child_pid, SIGKILL);
        } else {
            error = std::string("exec confirmation pipe failed: ") + std::strerror(pipe_error_number);
            kill(child_pid, SIGKILL);
        }

        ReapChildBlocking(child_pid);

        std::scoped_lock state_lock(state_mutex_);
        TransitionStateLocked(ServiceState::kFailed, error);
        return false;
    }

    {
        std::scoped_lock state_lock(state_mutex_);
        pid_ = child_pid;
        process_group_id_ = child_pid;
        start_time_ = std::chrono::steady_clock::now();
    }

    try {
        observer_thread_ = std::thread(&ProcessSupervisor::ObserveChildExit, this, child_pid, child_pid);
    } catch (const std::system_error& exception) {
        error = std::string("failed to start child exit observer: ") + exception.what();
        SignalProcessTree(child_pid, child_pid, SIGKILL);
        ReapChildBlocking(child_pid);

        std::scoped_lock state_lock(state_mutex_);
        if (pid_ == child_pid) {
            pid_ = -1;
            process_group_id_ = -1;
            start_time_.reset();
            TransitionStateLocked(ServiceState::kFailed, error);
        }
        return false;
    }

    {
        std::scoped_lock state_lock(state_mutex_);
        if (pid_ == child_pid && state_ == ServiceState::kStarting) {
            TransitionStateLocked(ServiceState::kRunning);
        }
    }

    return true;
}
bool ProcessSupervisor::StopOperation(std::string& error, const DesiredState final_desired_state) {
    error.clear();

    pid_t target_pid = -1;
    pid_t target_process_group_id = -1;

    {
        std::scoped_lock state_lock(state_mutex_);

        desired_state_ = final_desired_state;

        if (pid_ <= 0 && process_group_id_ <= 0) {
            if (final_desired_state == DesiredState::kStopped && state_ != ServiceState::kStopped) {
                TransitionStateLocked(ServiceState::kStopped);
            }
        } else {
            target_pid = pid_;
            target_process_group_id = process_group_id_;
            TransitionStateLocked(ServiceState::kStopping);
        }
    }

    if (target_pid <= 0 && target_process_group_id <= 0) {
        JoinObserverThread();
        return true;
    }

    if (SignalProcessTree(target_process_group_id, target_pid, SIGTERM) != 0 && errno != ESRCH) {
        error = std::string("SIGTERM process tree failed: ") + std::strerror(errno);

        std::scoped_lock state_lock(state_mutex_);
        if (pid_ == target_pid) {
            TransitionStateLocked(ServiceState::kFailed, error);
        }
        return false;
    }

    if (WaitForObservedExitAndProcessGroup(target_pid, target_process_group_id,
                                           std::chrono::steady_clock::now() + kGracefulStopTimeout)) {
        JoinObserverThread();
        return true;
    }

    if (SignalProcessTree(target_process_group_id, target_pid, SIGKILL) != 0 && errno != ESRCH) {
        error = std::string("SIGKILL process tree failed: ") + std::strerror(errno);

        std::scoped_lock state_lock(state_mutex_);
        if (pid_ == target_pid) {
            TransitionStateLocked(ServiceState::kFailed, error);
        }
        return false;
    }

    if (WaitForObservedExitAndProcessGroup(target_pid, target_process_group_id,
                                           std::chrono::steady_clock::now() + kForcedStopTimeout)) {
        JoinObserverThread();
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
bool ProcessSupervisor::WaitForObservedExitAndProcessGroup(const pid_t target_pid, const pid_t target_process_group_id,
                                                           const std::chrono::steady_clock::time_point deadline) {
    std::unique_lock state_lock(state_mutex_);

    while (true) {
        const bool leader_was_observed = target_pid <= 0 || pid_ != target_pid;
        state_lock.unlock();
        const bool process_group_is_gone = !ProcessGroupExists(target_process_group_id);
        state_lock.lock();

        if (leader_was_observed && process_group_is_gone) {
            if (process_group_id_ == target_process_group_id) {
                process_group_id_ = -1;
            }
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }

        exit_condition_.wait_until(state_lock, std::min(deadline, now + kProcessGroupPollInterval));
    }
}
void ProcessSupervisor::ObserveChildExit(const pid_t target_pid, const pid_t target_process_group_id) noexcept {
    int status = 0;
    pid_t result = -1;

    do {
        result = waitpid(target_pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    const int wait_error_number = result < 0 ? errno : 0;

    {
        std::scoped_lock state_lock(state_mutex_);

        if (pid_ != target_pid) {
            exit_condition_.notify_all();
            return;
        }

        if (result == target_pid) {
            SaveExitStatusLocked(status);
            MarkChildExitedLocked();
        } else if (result < 0 && wait_error_number == ECHILD) {
            last_exit_code_.reset();
            last_exit_kind_ = ProcessExitKind::kUnknown;
            last_exit_signal_.reset();
            MarkChildExitedLocked();
        } else {
            pid_ = -1;
            start_time_.reset();
            last_exit_code_.reset();
            last_exit_kind_ = ProcessExitKind::kUnknown;
            last_exit_signal_.reset();
            TransitionStateLocked(ServiceState::kFailed,
                                  std::string("waitpid failed for service child: ") + std::strerror(wait_error_number));
        }
    }

    exit_condition_.notify_all();
    CleanupProcessGroupAfterLeaderExit(target_process_group_id);
}
void ProcessSupervisor::CleanupProcessGroupAfterLeaderExit(const pid_t target_process_group_id) noexcept {
    if (ProcessGroupExists(target_process_group_id)) {
        SignalProcessTree(target_process_group_id, -1, SIGTERM);

        if (!WaitForProcessGroupExit(target_process_group_id,
                                     std::chrono::steady_clock::now() + kGracefulStopTimeout)) {
            SignalProcessTree(target_process_group_id, -1, SIGKILL);
            WaitForProcessGroupExit(target_process_group_id, std::chrono::steady_clock::now() + kForcedStopTimeout);
        }
    }

    {
        std::scoped_lock state_lock(state_mutex_);
        if (process_group_id_ == target_process_group_id && !ProcessGroupExists(target_process_group_id)) {
            process_group_id_ = -1;
        }
    }

    exit_condition_.notify_all();
}
void ProcessSupervisor::JoinObserverThread() noexcept {
    if (observer_thread_.joinable()) {
        observer_thread_.join();
    }
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
