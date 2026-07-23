#include "performance/http_benchmark_runner.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct CommandLineOptions {
    std::filesystem::path output_path;
    std::size_t repetitions{3};
};

[[nodiscard]] std::string PlatformName() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

[[nodiscard]] std::string ArchitectureName() {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string CompilerName() {
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    return "Unknown";
#endif
}

[[nodiscard]] std::string BuildType() {
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
}

[[nodiscard]] std::string GeneratedAtUtc() {
    const std::time_t current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
#if defined(_WIN32)
    gmtime_s(&utc_time, &current_time);
#else
    gmtime_r(&current_time, &utc_time);
#endif

    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%d %H:%M:%S UTC");
    return output.str();
}

[[nodiscard]] std::size_t ParsePositiveCount(const std::string_view value, const std::string_view option_name) {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(std::string(value), &consumed);
    if (consumed != value.size() || parsed == 0 || parsed > 20) {
        throw std::invalid_argument(std::string(option_name) + " must be between 1 and 20");
    }
    return static_cast<std::size_t>(parsed);
}

[[nodiscard]] CommandLineOptions ParseCommandLine(const int argc, char* argv[]) {
    CommandLineOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--output") {
            if (++index >= argc) {
                throw std::invalid_argument("--output requires a path");
            }
            options.output_path = argv[index];
        } else if (argument == "--repetitions") {
            if (++index >= argc) {
                throw std::invalid_argument("--repetitions requires a value");
            }
            options.repetitions = ParsePositiveCount(argv[index], "--repetitions");
        } else if (argument == "--help") {
            std::cout << "Usage: aegis_http_benchmark [--output REPORT.md] [--repetitions 1-20]\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

[[nodiscard]] aegis::test::HttpBenchmarkResult RunMedianBenchmark(const aegis::test::HttpBenchmarkScenario& scenario,
                                                                  const std::size_t repetitions) {
    std::vector<aegis::test::HttpBenchmarkResult> results;
    results.reserve(repetitions);
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
        results.push_back(aegis::test::RunHttpBenchmark(scenario));
    }

    std::ranges::sort(results, {}, &aegis::test::HttpBenchmarkResult::requests_per_second);
    return results[results.size() / 2U];
}

[[nodiscard]] bool Passed(const aegis::test::HttpBenchmarkResult& result) {
    return result.successful_requests == result.total_requests && result.failed_requests == 0
           && result.active_sessions_after_stop == 0 && result.in_flight_requests_after_stop == 0;
}

[[nodiscard]] std::string BuildReport(const std::vector<aegis::test::HttpBenchmarkResult>& results,
                                      const std::size_t repetitions) {
    std::ostringstream report;
    report << std::fixed << std::setprecision(2);
    report << "# AegisDesk HTTP Server 性能验收报告\n\n";
    report << "> 自动生成于 " << GeneratedAtUtc() << "。每个场景执行 " << repetitions
           << " 次，表格记录吞吐量居中的一次结果。\n\n";
    report << "## 测试环境\n\n";
    report << "| 项目 | 值 |\n";
    report << "| --- | --- |\n";
    report << "| 操作系统 | " << PlatformName() << " |\n";
    report << "| 架构 | " << ArchitectureName() << " |\n";
    report << "| 编译器 | " << CompilerName() << " |\n";
    report << "| 构建类型 | " << BuildType() << " |\n";
    report << "| 硬件并发数 | " << std::thread::hardware_concurrency() << " |\n\n";
    report << "## 验收结果\n\n";
    report << "| 场景 | 并发连接 | 请求数 | 成功率 | 吞吐量（req/s） | P50 | P95 | P99 | 最大并行 Handler | 状态 |\n";
    report << "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |\n";

    for (const aegis::test::HttpBenchmarkResult& result : results) {
        const double success_rate =
            100.0 * static_cast<double>(result.successful_requests) / static_cast<double>(result.total_requests);
        report << "| " << result.scenario.name << " | " << result.scenario.concurrent_clients << " | "
               << result.total_requests << " | " << success_rate << "% | " << result.requests_per_second << " | "
               << result.p50_milliseconds << "ms | " << result.p95_milliseconds << "ms | " << result.p99_milliseconds
               << "ms | " << result.maximum_active_handlers << " | " << (Passed(result) ? "通过" : "失败") << " |\n";
    }

    report << "\n## 场景说明\n\n";
    for (const aegis::test::HttpBenchmarkResult& result : results) {
        report << "- `" << result.scenario.name << "`：" << result.scenario.description << "\n";
    }
    report << "\n## 验收口径\n\n";
    report << "- 所有请求必须返回预期的 HTTP 200 JSON 响应。\n";
    report << "- Server 停止后，活跃 Session 和在途请求计数必须归零。\n";
    report << "- 延迟和吞吐量用于同一环境下的版本趋势对比，不作为跨机器的固定门槛。\n";
    report << "- CI 中的 `performance` 测试另行检查 50 并发、Keep-Alive 持续负载和 Handler 并行加速。\n";
    return report.str();
}

} // namespace

int main(const int argc, char* argv[]) {
    try {
        const CommandLineOptions options = ParseCommandLine(argc, argv);
        const std::vector<aegis::test::HttpBenchmarkScenario> scenarios{
            {
                .name = "short-connections",
                .description = "32 个客户端各创建 25 次短连接，包含 TCP 建连、请求和响应开销。",
                .concurrent_clients = 32,
                .requests_per_client = 25,
                .keep_alive = false,
                .handler_delay = std::chrono::milliseconds::zero(),
                .io_thread_count = 2,
                .handler_thread_count = 4,
            },
            {
                .name = "keep-alive",
                .description = "32 个客户端各复用一个连接完成 100 次请求，验证持续吞吐和连接复用。",
                .concurrent_clients = 32,
                .requests_per_client = 100,
                .keep_alive = true,
                .handler_delay = std::chrono::milliseconds::zero(),
                .io_thread_count = 2,
                .handler_thread_count = 4,
            },
            {
                .name = "handler-pool-5ms",
                .description = "16 个客户端持续请求，Handler 每次模拟 5ms 业务工作，验证有界线程池并行能力。",
                .concurrent_clients = 16,
                .requests_per_client = 10,
                .keep_alive = true,
                .handler_delay = std::chrono::milliseconds(5),
                .io_thread_count = 2,
                .handler_thread_count = 4,
            },
        };

        std::vector<aegis::test::HttpBenchmarkResult> results;
        results.reserve(scenarios.size());
        for (const aegis::test::HttpBenchmarkScenario& scenario : scenarios) {
            results.push_back(RunMedianBenchmark(scenario, options.repetitions));
        }

        const std::string report = BuildReport(results, options.repetitions);
        std::cout << report;

        if (!options.output_path.empty()) {
            const std::filesystem::path parent = options.output_path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            std::ofstream output(options.output_path);
            if (!output) {
                throw std::runtime_error("failed to open benchmark report: " + options.output_path.string());
            }
            output << report;
        }

        return std::ranges::all_of(results, Passed) ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "aegis_http_benchmark: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
