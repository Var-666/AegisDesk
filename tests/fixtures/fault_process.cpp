#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_running = 1;

extern "C" void StopHandler(int) {
    g_running = 0;
}

struct Options {
    int exit_code{0};
    std::optional<std::chrono::milliseconds> exit_after;
    std::optional<std::filesystem::path> ready_file;
    bool ignore_sigterm{false};
    bool spawn_child{false};
    bool leave_child_running{false};
};

template <typename Integer> bool ParseInteger(const std::string_view text, Integer& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

std::optional<Options> ParseOptions(const int argc, char* argv[]) {
    Options options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];

        const auto read_value = [&]() -> std::optional<std::string_view> {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            return argv[++index];
        };

        if (argument == "--exit-code") {
            const auto value = read_value();
            if (!value.has_value() || !ParseInteger(*value, options.exit_code) || options.exit_code < 0
                || options.exit_code > 255) {
                return std::nullopt;
            }
        } else if (argument == "--exit-after-ms") {
            long long milliseconds = 0;
            const auto value = read_value();
            if (!value.has_value() || !ParseInteger(*value, milliseconds) || milliseconds < 0) {
                return std::nullopt;
            }
            options.exit_after = std::chrono::milliseconds(milliseconds);
        } else if (argument == "--ready-file") {
            const auto value = read_value();
            if (!value.has_value() || value->empty()) {
                return std::nullopt;
            }
            options.ready_file = std::filesystem::path(*value);
        } else if (argument == "--ignore-sigterm") {
            options.ignore_sigterm = true;
        } else if (argument == "--spawn-child") {
            options.spawn_child = true;
        } else if (argument == "--leave-child-running") {
            options.leave_child_running = true;
        } else {
            return std::nullopt;
        }
    }

    if (options.leave_child_running && !options.spawn_child) {
        return std::nullopt;
    }

    return options;
}

void RunLoop(const std::optional<std::chrono::milliseconds> exit_after) {
    const auto started_at = std::chrono::steady_clock::now();

    while (g_running != 0) {
        if (exit_after.has_value() && std::chrono::steady_clock::now() - started_at >= *exit_after) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const auto options = ParseOptions(argc, argv);
    if (!options.has_value()) {
        std::cerr << "invalid fault process arguments\n";
        return 2;
    }

    if (options->ignore_sigterm) {
        std::signal(SIGTERM, SIG_IGN);
    } else {
        std::signal(SIGTERM, StopHandler);
    }
    std::signal(SIGINT, StopHandler);

    pid_t child_pid = -1;
    if (options->spawn_child) {
        child_pid = fork();
        if (child_pid < 0) {
            return 3;
        }
        if (child_pid == 0) {
            RunLoop(std::nullopt);
            _exit(0);
        }
    }

    if (options->ready_file.has_value()) {
        std::ofstream ready(*options->ready_file, std::ios::trunc);
        if (!ready.is_open()) {
            return 4;
        }
        ready << ::getpid() << '\n';
        if (child_pid > 0) {
            ready << child_pid << '\n';
        }
    }

    RunLoop(options->exit_after);

    if (child_pid > 0 && !options->leave_child_running) {
        kill(child_pid, SIGTERM);
        while (waitpid(child_pid, nullptr, 0) < 0 && errno == EINTR) {}
    }

    return options->exit_code;
}
