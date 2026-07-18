#pragma once

#include "agent/api/http_json.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace aegis::agent {

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

private:
    void ValidateOptions() const;

    [[nodiscard]] unsigned short PrepareAcceptor();

    void AcceptLoop() noexcept;

    void HandleSession(boost::asio::ip::tcp::socket socket) const;

    void CloseAcceptor() noexcept;

    void WaitNoThrow() noexcept;

private:
    HttpServerOptions options_;
    RequestHandler handler_;
    boost::asio::io_context io_context_{-1};
    boost::asio::ip::tcp::acceptor acceptor_{io_context_};

    std::atomic<unsigned short> bound_port_{0};
    std::atomic_bool stop_requested_{false};

    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    HttpServerState state_{HttpServerState::kStopped};
    bool worker_finished_{true};
    bool wait_in_progress_{false};
    std::exception_ptr worker_error_;
    std::thread worker_;
};

} // namespace aegis::agent
