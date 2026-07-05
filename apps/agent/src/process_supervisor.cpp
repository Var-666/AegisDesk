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

ProcessSupervisor::ProcessSupervisor(ServiceDefinition definition)
    : definition_(std::move(definition)) {
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
    std::scoped_lock lock(mutex_);

    error.clear();
    ReapExitedChildLocked();

    if (!definition_.IsStructurallyValid()) {
        error = "invalid service definition: " + definition_.id;
        return false;
    }

    if (pid_ > 0) {
        error = "demo_service is already running";
        return false;
    }

    if (!std::filesystem::exists(definition_.executable)) {
        error = "service executable does not exist: " + definition_.executable.string();
        return false;
    }

    if (!std::filesystem::exists(definition_.work_dir) || !std::filesystem::is_directory(definition_.work_dir)) {
        error = "service work directory does not exist or is not a directory: " + definition_.work_dir.string();
        return false;
    }

    // execv() 需要 C 风格 argv：
    // argv[0] = executable
    // argv[1..n] = ServiceDefinition.args
    // argv[n + 1] = nullptr
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
const ServiceDefinition& ProcessSupervisor::Definition() const noexcept {
    return definition_;
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
