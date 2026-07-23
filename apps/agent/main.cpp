#include "agent/api/agent_api.h"
#include "agent/api/http_server.h"
#include "agent/health/health_monitor.h"
#include "agent/metrics/metrics_collector.h"
#include "agent/service/service_registry.h"

#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void HandleSignal(int) {
    g_stop_requested = 1;
}

struct AgentOptions {
    std::filesystem::path config_path{"configs/services.json"};

    std::filesystem::path work_dir{"."};

    unsigned short port{18081};
};

void PrintUsage() {
    std::cout << "Usage: agent "
              << "[--config PATH] "
              << "[--work-dir PATH] "
              << "[--port PORT]\n";
}

std::optional<AgentOptions> ParseArgs(const int argc, char* argv[]) {
    AgentOptions options;

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
            return std::nullopt;
        }

        if (argument == "--config") {
            const auto value = read_value(argument);

            if (!value.has_value()) {
                return std::nullopt;
            }

            options.config_path = *value;
            continue;
        }

        if (argument == "--work-dir") {
            const auto value = read_value(argument);

            if (!value.has_value()) {
                return std::nullopt;
            }

            options.work_dir = *value;
            continue;
        }

        if (argument == "--port") {
            const auto value = read_value(argument);

            if (!value.has_value()) {
                return std::nullopt;
            }

            unsigned int parsed = 0;

            const auto [end, parse_error] = std::from_chars(value->data(), value->data() + value->size(), parsed);

            if (parse_error != std::errc{} || end != value->data() + value->size() || parsed == 0 || parsed > 65535) {
                std::cerr << "invalid port: " << *value << '\n';

                return std::nullopt;
            }

            options.port = static_cast<unsigned short>(parsed);
            continue;
        }

        std::cerr << "unknown argument: " << argument << '\n';

        return std::nullopt;
    }

    return options;
}

std::optional<std::filesystem::path> MakeAbsolutePath(const std::filesystem::path& path, const std::string_view label) {
    std::error_code error;

    const std::filesystem::path absolute_path = std::filesystem::absolute(path, error);

    if (error) {
        std::cerr << "failed to resolve " << label << ": " << error.message() << '\n';

        return std::nullopt;
    }

    return absolute_path;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::optional<AgentOptions> options = ParseArgs(argc, argv);

    if (!options.has_value()) {
        return 0;
    }

    const auto work_dir = MakeAbsolutePath(options->work_dir, "work directory");
    const auto config_path = MakeAbsolutePath(options->config_path, "config path");

    if (!work_dir.has_value() || !config_path.has_value()) {
        return 1;
    }

    if (!std::filesystem::exists(*work_dir) || !std::filesystem::is_directory(*work_dir)) {
        std::cerr << "work directory does not exist or is not a directory: " << *work_dir << '\n';
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    aegis::agent::ServiceRegistry registry;

    std::string registry_error;

    if (!registry.LoadFromFile(*config_path, *work_dir, registry_error)) {
        std::cerr << "[agent] failed to load services config: " << registry_error << '\n';

        return 1;
    }

    std::cout << "AegisDesk Agent loaded " << registry.Size() << " service definitions\n";

    for (const aegis::agent::ServiceSummary& service : registry.ListServices()) {
        std::cout << "  - " << service.id << " (" << service.display_name
                  << "), auto_start=" << (service.auto_start ? "true" : "false") << '\n';
    }

    if (!registry.StartAutoStartServices(registry_error)) {
        std::cerr << "[agent] " << registry_error << '\n';
    }

    aegis::agent::MetricsCollector metrics_collector(registry, {
                                                                   .interval = std::chrono::milliseconds(1000),
                                                               });

    std::string collector_error;

    if (!metrics_collector.Start(collector_error)) {
        std::cerr << "[agent] failed to start metrics collector: " << collector_error << '\n';

        std::string shutdown_error;
        registry.StopAll(shutdown_error);

        return 1;
    }

    aegis::agent::HealthMonitor health_monitor(registry, metrics_collector,
                                               {
                                                   .interval = std::chrono::milliseconds(2000),
                                                   .alert_manager_options =
                                                       {
                                                           .recent_resolved_capacity = 200,
                                                       },
                                                   .recovery_manager_options =
                                                       {
                                                           .recent_event_capacity = 200,
                                                           .suppress_event_cooldown_seconds = 30,
                                                       },
                                               });

    std::string health_monitor_error;

    if (!health_monitor.Start(health_monitor_error)) {
        std::cerr << "[agent] failed to start health monitor: " << health_monitor_error << '\n';

        metrics_collector.Stop();

        std::string shutdown_error;
        registry.StopAll(shutdown_error);

        return 1;
    }

    aegis::agent::AgentApi api(registry, metrics_collector, health_monitor);

    int exit_code = 0;

    try {
        aegis::agent::HttpServer server(
            {
                .bind_address = "127.0.0.1",
                .port = options->port,
            },
            [&api](const aegis::agent::HttpRequest& request) { return api.Handle(request); });

        const unsigned short bound_port = server.Start();

        std::cout << "AegisDesk Agent listening on http://127.0.0.1:" << bound_port << '\n';
        std::cout << "service config: " << *config_path << '\n';
        std::cout << "path base directory: " << *work_dir << '\n';

        while (g_stop_requested == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        server.RequestStop();
        server.Wait();
    } catch (const std::exception& exception) {
        std::cerr << "[agent] fatal error: " << exception.what() << '\n';
        exit_code = 1;
    }

    health_monitor.Stop();
    metrics_collector.Stop();

    std::string shutdown_error;
    if (!registry.StopAll(shutdown_error)) {
        std::cerr << "[agent] shutdown stop failed: " << shutdown_error << '\n';

        exit_code = 1;
    }

    return exit_code;
}
