#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
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

namespace {

using namespace std::chrono_literals;

volatile std::sig_atomic_t g_running = 1;

extern "C" void HandleSignal(int) {
    g_running = 0;
}

struct ServiceOptions {
    std::string service_name{"demo_service"};
    std::chrono::milliseconds heartbeat_interval{1000};
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
              << "[--interval-ms MILLISECONDS]\n\n"
              << "Examples:\n"
              << "  demo_service\n"
              << "  demo_service --name demo_worker --interval-ms 1500\n";
}

ParseResult ParseArguments(const int argc, char* argv[]) {
    ServiceOptions options;

    bool has_name = false;
    bool has_interval = false;

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
    std::signal(SIGTERM, HandleSignal);

    logger.Info("service started heartbeat_interval_ms=" + std::to_string(options.heartbeat_interval.count()));

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

        SleepInterruptibly(options.heartbeat_interval);
    }

    logger.Info("service stopping");
    logger.Info("service stopped");

    return 0;
}