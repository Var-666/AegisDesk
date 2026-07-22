#pragma once

#include "agent/api/http_json.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace aegis::agent {

class HttpSession;
class BoundedRequestExecutor;

enum class HttpServerState {
    kStopped,
    kStarting,
    kRunning,
    kStopping,
};

struct HttpServerOptions {
    std::string bind_address{"127.0.0.1"};
    unsigned short port{18081};

    std::size_t io_thread_count{1};
    std::size_t handler_thread_count{4};
    std::size_t max_connections{128};
    std::size_t max_in_flight_requests{64};

    std::size_t max_header_bytes{16 * 1024};
    std::size_t max_body_bytes{1024 * 1024};
    std::size_t max_requests_per_connection{100};

    std::chrono::seconds read_timeout{5};
    std::chrono::seconds write_timeout{15};
    std::chrono::seconds idle_timeout{30};
    std::chrono::milliseconds shutdown_grace_period{5000};
};

class HttpServer {
public:
    using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer(HttpServerOptions options, RequestHandler handler);

    ~HttpServer() noexcept;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] unsigned short Start();

    void RequestStop() noexcept;

    void Wait();

    void Run(const std::function<bool()>& stop_requested);

    [[nodiscard]] unsigned short BoundPort() const noexcept;

    [[nodiscard]] HttpServerState State() const noexcept;

    [[nodiscard]] const HttpServerOptions& Options() const noexcept;

    [[nodiscard]] std::size_t ActiveSessionCount() const noexcept;

    [[nodiscard]] std::size_t InFlightRequestCount() const noexcept;

private:
    void ValidateOptions() const;

    [[nodiscard]] unsigned short PrepareAcceptor();

    void StartAccept();

    void HandleAccept(const boost::system::error_code& error, boost::asio::ip::tcp::socket socket);

    [[nodiscard]] bool RegisterSession(const std::shared_ptr<HttpSession>& session);

    void RemoveSession(const std::shared_ptr<HttpSession>& session) noexcept;

    void StopOnIoContext() noexcept;

    void HandleShutdownDeadline(const boost::system::error_code& error) noexcept;

    void ForceCloseSessions() noexcept;

    void TryCompleteStopOnIoContext() noexcept;

    void RunIoContext() noexcept;

    void CloseAcceptor() noexcept;

    void WaitNoThrow() noexcept;

private:
    HttpServerOptions options_;
    RequestHandler handler_;
    boost::asio::io_context io_context_{-1};
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer shutdown_timer_;

    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    std::optional<WorkGuard> work_guard_;

    std::atomic<unsigned short> bound_port_{0};
    std::atomic_bool stop_requested_{false};
    std::atomic_bool handler_executor_drained_{false};
    std::atomic_size_t running_io_workers_{0};

    mutable std::mutex sessions_mutex_;
    std::unordered_set<std::shared_ptr<HttpSession>> sessions_;
    std::shared_ptr<BoundedRequestExecutor> request_executor_;
    bool stop_completion_started_{false};

    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    HttpServerState state_{HttpServerState::kStopped};
    bool wait_in_progress_{false};
    std::exception_ptr worker_error_;
    std::vector<std::thread> io_workers_;
};

} // namespace aegis::agent
