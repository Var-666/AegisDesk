//
// Created by Var on 2026/7/3.
//

#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include <sys/types.h>

namespace aegis::agent {
enum class ServiceState {
    kStopped,
    kRunning,
};

[[nodiscard]] std::string ToString(ServiceState state);

struct ServiceStatus {
    ServiceState state{ServiceState::kStopped};
    pid_t pid{-1};
    std::optional<int> exit_code;
    std::chrono::seconds uptime{0};
};

class ProcessSupervisor {
public:
    ProcessSupervisor(const std::filesystem::path& service_path, const std::filesystem::path& work_dir);

    ~ProcessSupervisor() noexcept;

    ProcessSupervisor(const ProcessSupervisor&) = delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

    bool Start(std::string& error);
    bool Stop(std::string& error);
    bool Restart(std::string& error);

    [[nodiscard]] ServiceStatus GetStatus();

private:
    bool ReapExitedChildLocked();
    void SaveExitStatusLocked(int status);

    std::filesystem::path service_path_;
    std::filesystem::path work_dir_;

    std::mutex mutex_;

    pid_t pid_{-1};
    std::optional<int> last_exit_code_;
    std::optional<std::chrono::steady_clock::time_point> start_time_;
};
} // namespace aegis::agent
