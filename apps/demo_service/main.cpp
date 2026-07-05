#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t g_running = 1;

extern "C" void HandleSignal(int) {
    g_running = 0;
}

//  获取当前时间
std::string Now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);

    std::tm local_time{};

#ifdef _WIN32
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif

    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

class Logger {
public:
    explicit Logger(const std::filesystem::path& path)
        : file_(path, std::ios::app) {
        if (!file_.is_open()) {
            std::cerr << "failed to open log file: " << path << '\n';
        }
    }

    void Info(const std::string& message) {
        Write("INFO", message);
    }

    void Warn(const std::string& message) {
        Write("WARN", message);
    }

private:
    void Write(const std::string& level, const std::string& message) {
        const std::string line = Now() + " " + level + " demo_service " + message;

        std::cout << line << '\n';

        if (file_.is_open()) {
            file_ << line << '\n';
            file_.flush();
        }
    }

    std::ofstream file_;
};
} // namespace

int main() {
    const std::filesystem::path project_root = AEGISDESK_PROJECT_ROOT;
    const std::filesystem::path log_dir = project_root / "runtime" / "logs";

    std::filesystem::create_directories(log_dir);

    Logger logger(log_dir / "demo_service.log");

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    logger.Info("service started");

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

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    logger.Info("service stopping");
    logger.Info("service stopped");

    return 0;
}