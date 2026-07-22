#include "agent/api/http_server.h"

#include "http_session.h"

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/bind_handler.hpp>

#include <algorithm>
#include <climits>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace aegis::agent {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

constexpr auto kRunStopPollInterval = std::chrono::milliseconds(20);

void CloseSocket(tcp::socket& socket) noexcept {
    beast::error_code ignored_error;
    socket.cancel(ignored_error);
    socket.shutdown(tcp::socket::shutdown_both, ignored_error);
    socket.close(ignored_error);
}

} // namespace

HttpServer::HttpServer(HttpServerOptions options, RequestHandler handler)
    : options_(std::move(options))
    , handler_(std::move(handler))
    , acceptor_(asio::make_strand(io_context_)) {}

HttpServer::~HttpServer() noexcept {
    RequestStop();
    WaitNoThrow();
}

unsigned short HttpServer::Start() {
    std::unique_lock lock(lifecycle_mutex_);

    if (state_ != HttpServerState::kStopped || !io_workers_.empty() || wait_in_progress_) {
        throw std::logic_error("HTTP server is already started or has not finished stopping");
    }

    ValidateOptions();

    state_ = HttpServerState::kStarting;
    stop_requested_.store(false, std::memory_order_release);
    running_io_workers_.store(0, std::memory_order_release);
    bound_port_.store(0, std::memory_order_release);
    worker_error_ = nullptr;

    try {
        const unsigned short port = PrepareAcceptor();
        work_guard_.emplace(io_context_.get_executor());
        io_workers_.reserve(options_.io_thread_count);

        for (std::size_t index = 0; index < options_.io_thread_count; ++index) {
            running_io_workers_.fetch_add(1, std::memory_order_acq_rel);

            try {
                io_workers_.emplace_back(&HttpServer::RunIoContext, this);
            } catch (...) {
                running_io_workers_.fetch_sub(1, std::memory_order_acq_rel);
                throw;
            }
        }

        bound_port_.store(port, std::memory_order_release);
        StartAccept();
        state_ = HttpServerState::kRunning;
        return port;
    } catch (...) {
        const std::exception_ptr start_error = std::current_exception();

        stop_requested_.store(true, std::memory_order_release);
        work_guard_.reset();
        io_context_.stop();

        std::vector<std::thread> workers_to_join = std::move(io_workers_);
        lock.unlock();

        for (std::thread& worker : workers_to_join) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        lock.lock();
        CloseAcceptor();
        running_io_workers_.store(0, std::memory_order_release);
        bound_port_.store(0, std::memory_order_release);
        state_ = HttpServerState::kStopped;
        lifecycle_condition_.notify_all();
        std::rethrow_exception(start_error);
    }
}

void HttpServer::RequestStop() noexcept {
    bool schedule_stop = false;

    {
        std::scoped_lock lock(lifecycle_mutex_);

        if (state_ == HttpServerState::kStopped) {
            return;
        }

        if (state_ != HttpServerState::kStopping) {
            state_ = HttpServerState::kStopping;
            schedule_stop = true;
        }

        stop_requested_.store(true, std::memory_order_release);
    }

    if (schedule_stop) {
        try {
            asio::post(acceptor_.get_executor(), [this] { StopOnIoContext(); });
        } catch (...) {
            io_context_.stop();
        }
    }

    lifecycle_condition_.notify_all();
}

void HttpServer::Wait() {
    std::vector<std::thread> workers_to_join;

    {
        std::unique_lock lock(lifecycle_mutex_);

        for (const std::thread& worker : io_workers_) {
            if (worker.joinable() && worker.get_id() == std::this_thread::get_id()) {
                throw std::logic_error("HTTP server I/O worker cannot wait for itself");
            }
        }

        lifecycle_condition_.wait(lock, [this] { return !wait_in_progress_; });

        if (io_workers_.empty()) {
            const std::exception_ptr worker_error = worker_error_;
            lock.unlock();

            if (worker_error != nullptr) {
                std::rethrow_exception(worker_error);
            }
            return;
        }

        wait_in_progress_ = true;
        workers_to_join = std::move(io_workers_);
    }

    for (std::thread& worker : workers_to_join) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::scoped_lock lock(sessions_mutex_);
        sessions_.clear();
    }

    std::exception_ptr worker_error;

    {
        std::scoped_lock lock(lifecycle_mutex_);
        work_guard_.reset();
        CloseAcceptor();
        io_context_.stop();
        running_io_workers_.store(0, std::memory_order_release);
        bound_port_.store(0, std::memory_order_release);
        state_ = HttpServerState::kStopped;
        wait_in_progress_ = false;
        worker_error = worker_error_;
    }

    lifecycle_condition_.notify_all();

    if (worker_error != nullptr) {
        std::rethrow_exception(worker_error);
    }
}

unsigned short HttpServer::BoundPort() const noexcept {
    return bound_port_.load(std::memory_order_acquire);
}

HttpServerState HttpServer::State() const noexcept {
    std::scoped_lock lock(lifecycle_mutex_);
    return state_;
}

const HttpServerOptions& HttpServer::Options() const noexcept {
    return options_;
}

std::size_t HttpServer::ActiveSessionCount() const noexcept {
    std::scoped_lock lock(sessions_mutex_);
    return sessions_.size();
}

void HttpServer::Run(const std::function<bool()>& stop_requested) {
    static_cast<void>(Start());

    std::exception_ptr run_error;

    try {
        while (!stop_requested()) {
            std::unique_lock lock(lifecycle_mutex_);
            if (lifecycle_condition_.wait_for(lock, kRunStopPollInterval, [this] {
                    return running_io_workers_.load(std::memory_order_acquire) == 0;
                })) {
                break;
            }
        }
    } catch (...) {
        run_error = std::current_exception();
    }

    RequestStop();

    try {
        Wait();
    } catch (...) {
        if (run_error == nullptr) {
            run_error = std::current_exception();
        }
    }

    if (run_error != nullptr) {
        std::rethrow_exception(run_error);
    }
}

void HttpServer::ValidateOptions() const {
    if (!handler_) {
        throw std::invalid_argument("HTTP server request handler must not be empty");
    }

    if (options_.bind_address.empty()) {
        throw std::invalid_argument("HTTP server bind address must not be empty");
    }

    if (options_.io_thread_count == 0 || options_.handler_thread_count == 0) {
        throw std::invalid_argument("HTTP server thread counts must be greater than zero");
    }

    if (options_.max_connections == 0 || options_.max_in_flight_requests == 0
        || options_.max_in_flight_requests > options_.max_connections) {
        throw std::invalid_argument(
            "HTTP server request limits must be positive and in-flight requests must not exceed connections");
    }

    if (options_.max_header_bytes == 0 || options_.max_body_bytes == 0 || options_.max_requests_per_connection == 0) {
        throw std::invalid_argument("HTTP server protocol limits must be greater than zero");
    }

    if (options_.read_timeout <= std::chrono::seconds::zero() || options_.write_timeout <= std::chrono::seconds::zero()
        || options_.idle_timeout <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("HTTP server timeouts must be greater than zero");
    }
}

unsigned short HttpServer::PrepareAcceptor() {
    io_context_.restart();

    beast::error_code error;

    const asio::ip::address address = asio::ip::make_address(options_.bind_address, error);
    if (error) {
        throw std::runtime_error("invalid bind address: " + error.message());
    }

    acceptor_.open(address.is_v4() ? tcp::v4() : tcp::v6(), error);
    if (error) {
        throw std::runtime_error("acceptor open failed: " + error.message());
    }

    acceptor_.set_option(asio::socket_base::reuse_address(true), error);
    if (error) {
        throw std::runtime_error("acceptor set_option failed: " + error.message());
    }

    acceptor_.bind({address, options_.port}, error);
    if (error) {
        throw std::runtime_error("acceptor bind failed: " + error.message());
    }

    const std::size_t capped_backlog = std::min(options_.max_connections, static_cast<std::size_t>(INT_MAX));
    acceptor_.listen(static_cast<int>(capped_backlog), error);
    if (error) {
        throw std::runtime_error("acceptor listen failed: " + error.message());
    }

    const tcp::endpoint local_endpoint = acceptor_.local_endpoint(error);
    if (error) {
        throw std::runtime_error("acceptor local_endpoint failed: " + error.message());
    }

    return local_endpoint.port();
}

void HttpServer::StartAccept() {
    if (stop_requested_.load(std::memory_order_acquire) || !acceptor_.is_open()) {
        return;
    }

    acceptor_.async_accept(asio::make_strand(io_context_), beast::bind_front_handler(&HttpServer::HandleAccept, this));
}

void HttpServer::HandleAccept(const beast::error_code& error, tcp::socket socket) {
    if (!error && !stop_requested_.load(std::memory_order_acquire)) {
        auto session = std::make_shared<HttpSession>(
            std::move(socket), options_, handler_,
            [this](const std::shared_ptr<HttpSession>& closed_session) { RemoveSession(closed_session); });

        if (RegisterSession(session)) {
            session->Start();
        } else {
            session->RequestStop();
        }
    } else if (!error) {
        CloseSocket(socket);
    } else if (error != asio::error::operation_aborted && error != asio::error::bad_descriptor) {
        std::cerr << "[agent] accept failed: " << error.message() << '\n';
    }

    if (!stop_requested_.load(std::memory_order_acquire)) {
        StartAccept();
    }
}

bool HttpServer::RegisterSession(const std::shared_ptr<HttpSession>& session) {
    std::scoped_lock lock(sessions_mutex_);

    if (stop_requested_.load(std::memory_order_acquire) || sessions_.size() >= options_.max_connections) {
        return false;
    }

    sessions_.insert(session);
    return true;
}

void HttpServer::RemoveSession(const std::shared_ptr<HttpSession>& session) noexcept {
    std::scoped_lock lock(sessions_mutex_);
    sessions_.erase(session);
}

void HttpServer::StopOnIoContext() noexcept {
    CloseAcceptor();

    std::vector<std::shared_ptr<HttpSession>> sessions;
    {
        std::scoped_lock lock(sessions_mutex_);
        sessions.assign(sessions_.begin(), sessions_.end());
    }

    for (const std::shared_ptr<HttpSession>& session : sessions) {
        session->RequestStop();
    }

    work_guard_.reset();
}

void HttpServer::RunIoContext() noexcept {
    std::exception_ptr worker_error;

    try {
        io_context_.run();
    } catch (...) {
        worker_error = std::current_exception();
    }

    if (worker_error != nullptr) {
        {
            std::scoped_lock lock(lifecycle_mutex_);
            if (worker_error_ == nullptr) {
                worker_error_ = worker_error;
            }
            if (state_ != HttpServerState::kStopped) {
                state_ = HttpServerState::kStopping;
            }
            stop_requested_.store(true, std::memory_order_release);
        }

        io_context_.stop();
    }

    running_io_workers_.fetch_sub(1, std::memory_order_acq_rel);
    lifecycle_condition_.notify_all();
}

void HttpServer::CloseAcceptor() noexcept {
    if (!acceptor_.is_open()) {
        return;
    }

    beast::error_code ignored_error;
    acceptor_.cancel(ignored_error);
    acceptor_.close(ignored_error);
}

void HttpServer::WaitNoThrow() noexcept {
    try {
        Wait();
    } catch (...) {}
}

} // namespace aegis::agent
