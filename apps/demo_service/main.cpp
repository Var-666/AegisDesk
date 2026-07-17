#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

volatile std::sig_atomic_t g_running = 1;

extern "C" void HandleSignal(int) {
    g_running = 0;
}

struct ServiceOptions {
    std::string service_name{"demo_service"};
    std::chrono::milliseconds heartbeat_interval{1000};
    std::optional<std::chrono::seconds> exit_after;
    int exit_code{0};
    bool ignore_sigterm{false};
    bool spawn_child{false};
    unsigned int cpu_load_percent{0};
};

enum class ParseResultKind {
    kRun,
    kHelp,
    kError,
};

struct ParseResult {
    ParseResultKind kind{ParseResultKind::kError};
    ServiceOptions options;
};

[[nodiscard]] bool IsAllowedServiceNameChar(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
           || (character >= '0' && character <= '9') || character == '_' || character == '-';
}

[[nodiscard]] bool IsValidServiceName(const std::string_view service_name) noexcept {
    constexpr std::size_t kMaxServiceNameLength = 64;

    if (service_name.empty() || service_name.size() > kMaxServiceNameLength) {
        return false;
    }

    for (const char character : service_name) {
        if (!IsAllowedServiceNameChar(character)) {
            return false;
        }
    }

    return true;
}

void PrintUsage() {
    std::cout << "Usage: demo_service "
              << "[--name SERVICE_NAME] "
              << "[--interval-ms MILLISECONDS] "
              << "[--exit-after SECONDS] "
              << "[--exit-code CODE] "
              << "[--ignore-sigterm] "
              << "[--spawn-child] "
              << "[--cpu-load-percent PERCENT]\n\n"
              << "Examples:\n"
              << "  demo_service\n"
              << "  demo_service --name demo_worker --interval-ms 1500\n"
              << "  demo_service --exit-after 5 --exit-code 23\n"
              << "  demo_service --ignore-sigterm --spawn-child\n"
              << "  demo_service --cpu-load-percent 60\n";
}

ParseResult ParseArguments(const int argc, char* argv[]) {
    ServiceOptions options;

    bool has_name = false;
    bool has_interval = false;
    bool has_exit_after = false;
    bool has_exit_code = false;
    bool has_ignore_sigterm = false;
    bool has_spawn_child = false;
    bool has_cpu_load_percent = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];

        const auto read_value = [&](const std::string_view option_name) -> std::optional<std::string_view> {
            if (index + 1 >= argc) {
                std::cerr << option_name << " requires a value\n";

                return std::nullopt;
            }

            return argv[++index];
        };

        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return {.kind = ParseResultKind::kHelp};
        }

        if (argument == "--name") {
            if (has_name) {
                std::cerr << "--name cannot be specified twice\n";
                return {.kind = ParseResultKind::kError};
            }

            const auto value = read_value(argument);

            if (!value.has_value()) {
                return {.kind = ParseResultKind::kError};
            }

            if (!IsValidServiceName(*value)) {
                std::cerr << "invalid service name: " << *value << "\nallowed characters: "
                          << "letters, digits, '_' and '-'\n";

                return {.kind = ParseResultKind::kError};
            }

            options.service_name = std::string(*value);
            has_name = true;
            continue;
        }

        if (argument == "--interval-ms") {
            if (has_interval) {
                std::cerr << "--interval-ms cannot be specified twice\n";

                return {.kind = ParseResultKind::kError};
            }

            const auto value = read_value(argument);

            if (!value.has_value()) {
                return {.kind = ParseResultKind::kError};
            }

            unsigned long long parsed = 0;

            const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);

            constexpr unsigned long long kMinIntervalMs = 100;
            constexpr unsigned long long kMaxIntervalMs = 60000;

            if (error != std::errc{} || end != value->data() + value->size() || parsed < kMinIntervalMs
                || parsed > kMaxIntervalMs) {
                std::cerr << "invalid --interval-ms value: " << *value << "\nallowed range: " << kMinIntervalMs
                          << " to " << kMaxIntervalMs << '\n';

                return {.kind = ParseResultKind::kError};
            }

            options.heartbeat_interval = std::chrono::milliseconds(parsed);

            has_interval = true;
            continue;
        }

        if (argument == "--exit-after") {
            if (has_exit_after) {
                std::cerr << "--exit-after cannot be specified twice\n";
                return {.kind = ParseResultKind::kError};
            }

            const auto value = read_value(argument);
            unsigned long long parsed = 0;
            constexpr unsigned long long kMaxExitAfterSeconds = 86400;

            if (!value.has_value()) {
                return {.kind = ParseResultKind::kError};
            }

            const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
            if (error != std::errc{} || end != value->data() + value->size() || parsed == 0
                || parsed > kMaxExitAfterSeconds) {
                std::cerr << "invalid --exit-after value: " << *value << "\nallowed range: 1 to "
                          << kMaxExitAfterSeconds << " seconds\n";
                return {.kind = ParseResultKind::kError};
            }

            options.exit_after = std::chrono::seconds(parsed);
            has_exit_after = true;
            continue;
        }

        if (argument == "--exit-code") {
            if (has_exit_code) {
                std::cerr << "--exit-code cannot be specified twice\n";
                return {.kind = ParseResultKind::kError};
            }

            const auto value = read_value(argument);
            unsigned int parsed = 0;

            if (!value.has_value()) {
                return {.kind = ParseResultKind::kError};
            }

            const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
            if (error != std::errc{} || end != value->data() + value->size() || parsed > 255) {
                std::cerr << "invalid --exit-code value: " << *value << "\nallowed range: 0 to 255\n";
                return {.kind = ParseResultKind::kError};
            }

            options.exit_code = static_cast<int>(parsed);
            has_exit_code = true;
            continue;
        }

        if (argument == "--ignore-sigterm") {
            if (has_ignore_sigterm) {
                std::cerr << "--ignore-sigterm cannot be specified twice\n";
                return {.kind = ParseResultKind::kError};
            }
            options.ignore_sigterm = true;
            has_ignore_sigterm = true;
            continue;
        }

        if (argument == "--spawn-child") {
            if (has_spawn_child) {
                std::cerr << "--spawn-child cannot be specified twice\n";
                return {.kind = ParseResultKind::kError};
            }
            options.spawn_child = true;
            has_spawn_child = true;
            continue;
        }

        if (argument == "--cpu-load-percent") {
            if (has_cpu_load_percent) {
                std::cerr << "--cpu-load-percent cannot be specified twice\n";
                return {.kind = ParseResultKind::kError};
            }

            const auto value = read_value(argument);
            unsigned int parsed = 0;

            if (!value.has_value()) {
                return {.kind = ParseResultKind::kError};
            }

            const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
            if (error != std::errc{} || end != value->data() + value->size() || parsed > 100) {
                std::cerr << "invalid --cpu-load-percent value: " << *value << "\nallowed range: 0 to 100\n";
                return {.kind = ParseResultKind::kError};
            }

            options.cpu_load_percent = parsed;
            has_cpu_load_percent = true;
            continue;
        }

        std::cerr << "unknown argument: " << argument << '\n';
        return {.kind = ParseResultKind::kError};
    }

    return {
        .kind = ParseResultKind::kRun,
        .options = std::move(options),
    };
}

std::string Now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);

    std::tm local_time{};
    localtime_r(&timestamp, &local_time);

    std::ostringstream output;
    output << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");

    return output.str();
}

class Logger {
public:
    Logger(std::filesystem::path path, std::string service_name)
        : file_(std::move(path), std::ios::app)
        , service_name_(std::move(service_name)) {}

    [[nodiscard]] bool IsOpen() const noexcept {
        return file_.is_open();
    }

    void Info(const std::string_view message) {
        Write("INFO", message);
    }

    void Warn(const std::string_view message) {
        Write("WARN", message);
    }

private:
    void Write(const std::string_view level, const std::string_view message) {
        const std::string line = Now() + " " + std::string(level) + " " + service_name_ + " " + std::string(message);

        std::cout << line << '\n';

        file_ << line << '\n';
        file_.flush();
    }

    std::ofstream file_;
    std::string service_name_;
};

void SleepInterruptibly(std::chrono::milliseconds duration) {
    constexpr auto kMaxSleepSlice = 100ms;

    while (g_running != 0 && duration > 0ms) {
        const auto sleep_time = std::min(duration, kMaxSleepSlice);

        std::this_thread::sleep_for(sleep_time);
        duration -= sleep_time;
    }
}

void RunCpuLoadInterruptibly(std::chrono::milliseconds duration, const unsigned int cpu_load_percent) {
    constexpr auto kCycle = 100ms;

    while (g_running != 0 && duration > 0ms) {
        const auto cycle = std::min(duration, kCycle);
        const auto busy_duration = cycle * cpu_load_percent / 100;
        const auto busy_until = std::chrono::steady_clock::now() + busy_duration;

        while (g_running != 0 && std::chrono::steady_clock::now() < busy_until) {}

        SleepInterruptibly(cycle - busy_duration);
        duration -= cycle;
    }
}

void StopAndReapChild(const pid_t child_pid, const bool child_ignores_sigterm) {
    if (child_pid <= 0) {
        return;
    }

    kill(child_pid, child_ignores_sigterm ? SIGKILL : SIGTERM);
    while (waitpid(child_pid, nullptr, 0) < 0 && errno == EINTR) {}
}

} // namespace

int main(int argc, char* argv[]) {
    const ParseResult parse_result = ParseArguments(argc, argv);

    if (parse_result.kind == ParseResultKind::kHelp) {
        return 0;
    }

    if (parse_result.kind == ParseResultKind::kError) {
        PrintUsage();
        return 1;
    }

    const ServiceOptions& options = parse_result.options;

    const std::filesystem::path log_directory = "runtime/logs";

    std::error_code error;

    std::filesystem::create_directories(log_directory, error);

    if (error) {
        std::cerr << "failed to create log directory: " << error.message() << '\n';

        return 1;
    }

    const std::filesystem::path log_path = log_directory / (options.service_name + ".log");

    Logger logger(log_path, options.service_name);

    if (!logger.IsOpen()) {
        std::cerr << "failed to open log file: " << log_path << '\n';

        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, options.ignore_sigterm ? SIG_IGN : HandleSignal);

    pid_t child_pid = -1;
    if (options.spawn_child) {
        child_pid = fork();
        if (child_pid < 0) {
            logger.Warn("failed to spawn child: " + std::string(std::strerror(errno)));
            return 2;
        }
        if (child_pid == 0) {
            while (g_running != 0) {
                std::this_thread::sleep_for(100ms);
            }
            _exit(0);
        }
        logger.Info("child spawned pid=" + std::to_string(child_pid));
    }

    logger.Info("service started heartbeat_interval_ms=" + std::to_string(options.heartbeat_interval.count())
                + " cpu_load_percent=" + std::to_string(options.cpu_load_percent));

    const auto started_at = std::chrono::steady_clock::now();
    const std::optional<std::chrono::steady_clock::time_point> exit_at =
        options.exit_after.has_value()
            ? std::optional<std::chrono::steady_clock::time_point>(started_at + *options.exit_after)
            : std::nullopt;

    int sequence = 0;

    while (g_running != 0) {
        ++sequence;

        logger.Info("heartbeat seq=" + std::to_string(sequence));

        if (sequence % 5 == 0) {
            logger.Info("request handled request_id=req-" + std::to_string(sequence) + " latency_ms=12");
        }

        if (sequence % 11 == 0) {
            logger.Warn("simulated slow request request_id=req-" + std::to_string(sequence) + " latency_ms=850");
        }

        auto work_duration = options.heartbeat_interval;
        if (exit_at.has_value()) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(*exit_at - std::chrono::steady_clock::now());
            if (remaining <= 0ms) {
                break;
            }
            work_duration = std::min(work_duration, remaining);
        }

        RunCpuLoadInterruptibly(work_duration, options.cpu_load_percent);

        if (exit_at.has_value() && std::chrono::steady_clock::now() >= *exit_at) {
            logger.Warn("fault injection exit-after reached exit_code=" + std::to_string(options.exit_code));
            break;
        }
    }

    logger.Info("service stopping");
    StopAndReapChild(child_pid, options.ignore_sigterm);
    logger.Info("service stopped");

    return options.exit_code;
}
