//
// Created by Var on 2026/7/3.
//

#include "agent/process_supervisor.h"

#include <csignal>
#include <iostream>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace aegis::agent {

std::string ToString(const ServiceState state) {
    switch (state) {
        case ServiceState::kStopped:
            return "Stopped";
        case ServiceState::kRunning:
            return "Running";
    }
    return "Unknown";
}
ProcessSupervisor::ProcessSupervisor(const std::filesystem::path& service_path, const std::filesystem::path& work_dir)
    : service_path_(std::filesystem::absolute(service_path))
    , work_dir_(std::filesystem::absolute(work_dir)) {}

ProcessSupervisor::~ProcessSupervisor() noexcept {
    std::string error;

    if (!Stop(error)) {
        std::cerr << "[agent] destructor stop failed: " << error << '\n';
    }
}
bool ProcessSupervisor::Start(std::string& error) {
    std::scoped_lock lock(mutex_);

    error.clear();
    ReapExitedChildLocked();

    if (pid_ > 0) {
        error = "demo_service is already running";
        return false;
    }

    if (!std::filesystem::exists(service_path_)) {
        error = "service executable does not exist: " + service_path_.string();
        return false;
    }

    const pid_t child_pid = fork();

    if (child_pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }

    if (child_pid == 0) {
        if (chdir(work_dir_.c_str()) != 0) {
            std::cerr << "agent child: chdir failed: " << std::strerror(errno) << '\n';
            _exit(127);
        }

        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        const std::string program_name = service_path_.filename().string();

        execl(service_path_.c_str(), program_name.c_str(), static_cast<char*>(nullptr));

        _exit(127);
    }

    pid_ = child_pid;
    last_exit_code_.reset();
    start_time_ = std::chrono::steady_clock::now();

    return true;
}
bool ProcessSupervisor::Stop(std::string& error) {
    std::scoped_lock lock(mutex_);

    error.clear();
    ReapExitedChildLocked();

    if (pid_ <= 0) {
        return true;
    }

    const pid_t target_pid = pid_;

    if (kill(target_pid, SIGTERM) != 0 && errno != ESRCH) {
        error = std::string("SIGTERM failed: ") + std::strerror(errno);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

    while (std::chrono::steady_clock::now() < deadline) {
        if (ReapExitedChildLocked()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (kill(target_pid, SIGKILL) != 0 && errno != ESRCH) {
        error = std::string("SIGKILL failed: ") + std::strerror(errno);
        return false;
    }

    int status = 0;

    while (true) {
        const pid_t result = waitpid(target_pid, &status, 0);

        if (result == target_pid) {
            SaveExitStatusLocked(status);
            break;
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result < 0 && errno == ECHILD) {
            last_exit_code_.reset();
            break;
        }

        error = std::string("waitpid failed: ") + std::strerror(errno);
        return false;
    }

    pid_ = -1;
    start_time_.reset();

    return true;
}
bool ProcessSupervisor::Restart(std::string& error) {
    if (!Stop(error)) {
        return false;
    }

    return Start(error);
}
ServiceStatus ProcessSupervisor::GetStatus() {
    std::scoped_lock lock(mutex_);

    ReapExitedChildLocked();

    ServiceStatus status;
    status.state = pid_ > 0 ? ServiceState::kRunning : ServiceState::kStopped;
    status.pid = pid_;
    status.exit_code = last_exit_code_;

    if (start_time_.has_value()) {
        status.uptime =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - *start_time_);
    }

    return status;
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
        pid_ = -1;
        start_time_.reset();
        return true;
    }

    if (result < 0 && errno == ECHILD) {
        pid_ = -1;
        last_exit_code_.reset();
        start_time_.reset();
        return true;
    }

    return false;
}
void ProcessSupervisor::SaveExitStatusLocked(int status) {
    if (WIFEXITED(status)) {
        last_exit_code_ = WEXITSTATUS(status);
        return;
    }

    if (WIFSIGNALED(status)) {
        last_exit_code_ = 128 + WTERMSIG(status);
        return;
    }

    last_exit_code_.reset();
}
} // namespace aegis::agent
