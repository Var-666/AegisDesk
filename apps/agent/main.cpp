#include "agent/agent_api.h"
#include "agent/http_server.h"
#include "agent/process_supervisor.h"

#include <charconv>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void HandleSignal(int) {
    g_stop_requested = 1;
}

struct AgentOptions {
    std::filesystem::path service_path{"build/apps/demo_service/demo_service"};

    std::filesystem::path work_dir{"."};

    unsigned short port{18081};
};

void PrintUsage() {
    std::cout << "Usage: agent "
              << "[--service PATH] "
              << "[--work-dir PATH] "
              << "[--port PORT]\n";
}

std::optional<AgentOptions> ParseArgs(const int argc, char* argv[]) {
    AgentOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];

        const auto read_value = [&](const std::string_view name) -> std::optional<std::string_view> {
            if (index + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return std::nullopt;
            }

            return argv[++index];
        };

        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return std::nullopt;
        }

        if (argument == "--service") {
            const auto value = read_value(argument);

            if (!value.has_value()) {
                return std::nullopt;
            }

            options.service_path = *value;
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

            const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);

            if (error != std::errc{} || end != value->data() + value->size() || parsed == 0 || parsed > 65535) {
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

} // namespace

int main(int argc, char* argv[]) {
    const std::optional<AgentOptions> options = ParseArgs(argc, argv);

    if (!options.has_value()) {
        return 0;
    }

    const std::filesystem::path work_dir = std::filesystem::absolute(options->work_dir);

    const std::filesystem::path service_path = std::filesystem::absolute(options->service_path);

    const std::filesystem::path log_path = work_dir / "runtime/logs/demo_service.log";

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    aegis::agent::ProcessSupervisor supervisor(service_path, work_dir);

    aegis::agent::AgentApi api(supervisor, log_path);

    try {
        aegis::agent::HttpServer server(
            {
                .bind_address = "127.0.0.1",
                .bind_port = options->port,
            },
            [&api](const aegis::agent::HttpRequest& request) { return api.Handle(request); });

        std::cout << "AegisDesk Agent listening on http://127.0.0.1:" << options->port << '\n';

        std::cout << "managed service: " << service_path << '\n';

        server.Run([] { return g_stop_requested != 0; });
    } catch (const std::exception& exception) {
        std::cerr << "[agent] fatal error: " << exception.what() << '\n';

        return 1;
    }

    std::string error;

    if (!supervisor.Stop(error)) {
        std::cerr << "[agent] shutdown stop failed: " << error << '\n';

        return 1;
    }

    return 0;
}