#include "agent/api/http_server.h"

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <climits>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace aegis::agent {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

constexpr auto kAcceptPollInterval = std::chrono::milliseconds(10);
constexpr auto kRunStopPollInterval = std::chrono::milliseconds(20);

} // namespace

HttpServer::HttpServer(HttpServerOptions options, RequestHandler handler)
    : options_(std::move(options))
    , handler_(std::move(handler)) {}

HttpServer::~HttpServer() noexcept {
    RequestStop();
    WaitNoThrow();
}

unsigned short HttpServer::Start() {
    std::unique_lock lock(lifecycle_mutex_);

    if (state_ != HttpServerState::kStopped || worker_.joinable() || wait_in_progress_) {
        throw std::logic_error("HTTP server is already started or has not finished stopping");
    }

    ValidateOptions();

    state_ = HttpServerState::kStarting;
    stop_requested_.store(false, std::memory_order_release);
    bound_port_.store(0, std::memory_order_release);
    worker_finished_ = false;
    worker_error_ = nullptr;

    try {
        const unsigned short port = PrepareAcceptor();
        bound_port_.store(port, std::memory_order_release);
        worker_ = std::thread(&HttpServer::AcceptLoop, this);
        state_ = HttpServerState::kRunning;
        return port;
    } catch (...) {
        stop_requested_.store(true, std::memory_order_release);
        CloseAcceptor();
        bound_port_.store(0, std::memory_order_release);
        worker_finished_ = true;
        state_ = HttpServerState::kStopped;
        lifecycle_condition_.notify_all();
        throw;
    }
}

void HttpServer::RequestStop() noexcept {
    {
        std::scoped_lock lock(lifecycle_mutex_);

        if (state_ == HttpServerState::kStopped) {
            return;
        }

        state_ = HttpServerState::kStopping;
        stop_requested_.store(true, std::memory_order_release);
    }

    lifecycle_condition_.notify_all();
}

void HttpServer::Wait() {
    std::thread worker_to_join;

    {
        std::unique_lock lock(lifecycle_mutex_);

        if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) {
            throw std::logic_error("HTTP server worker cannot wait for itself");
        }

        lifecycle_condition_.wait(lock, [this] { return !wait_in_progress_; });

        if (!worker_.joinable()) {
            const std::exception_ptr worker_error = worker_error_;
            lock.unlock();

            if (worker_error != nullptr) {
                std::rethrow_exception(worker_error);
            }
            return;
        }

        wait_in_progress_ = true;
        worker_to_join = std::move(worker_);
    }

    worker_to_join.join();

    std::exception_ptr worker_error;

    {
        std::scoped_lock lock(lifecycle_mutex_);
        CloseAcceptor();
        bound_port_.store(0, std::memory_order_release);
        worker_finished_ = true;
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

void HttpServer::Run(const std::function<bool()>& stop_requested) {
    static_cast<void>(Start());

    std::exception_ptr run_error;

    try {
        while (!stop_requested()) {
            std::unique_lock lock(lifecycle_mutex_);
            if (lifecycle_condition_.wait_for(lock, kRunStopPollInterval, [this] { return worker_finished_; })) {
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

    acceptor_.non_blocking(true, error);
    if (error) {
        throw std::runtime_error("acceptor non_blocking failed: " + error.message());
    }

    const tcp::endpoint local_endpoint = acceptor_.local_endpoint(error);
    if (error) {
        throw std::runtime_error("acceptor local_endpoint failed: " + error.message());
    }

    return local_endpoint.port();
}

void HttpServer::AcceptLoop() noexcept {
    std::exception_ptr worker_error;

    try {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            beast::error_code error;
            tcp::socket socket(io_context_);

            acceptor_.accept(socket, error);

            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(kAcceptPollInterval);
                continue;
            }

            if (error) {
                if (stop_requested_.load(std::memory_order_acquire)
                    && (error == asio::error::operation_aborted || error == asio::error::bad_descriptor)) {
                    break;
                }

                std::cerr << "[agent] accept failed: " << error.message() << '\n';
                std::this_thread::sleep_for(kAcceptPollInterval);
                continue;
            }

            HandleSession(std::move(socket));
        }
    } catch (...) {
        worker_error = std::current_exception();
        stop_requested_.store(true, std::memory_order_release);
    }

    {
        std::scoped_lock lock(lifecycle_mutex_);

        if (worker_error != nullptr) {
            worker_error_ = worker_error;
            state_ = HttpServerState::kStopping;
        }

        worker_finished_ = true;
    }

    lifecycle_condition_.notify_all();
}

void HttpServer::HandleSession(boost::asio::ip::tcp::socket socket) const {
    beast::tcp_stream stream(std::move(socket));

    stream.expires_after(options_.read_timeout);

    beast::flat_buffer buffer;
    HttpRequest request;

    beast::error_code error;

    http::read(stream, buffer, request, error);
    if (error == http::error::end_of_stream) {
        return;
    }
    if (error) {
        std::cerr << "[agent] http read failed: " << error.message() << '\n';
        return;
    }

    HttpResponse response;
    try {
        response = handler_(request);
    } catch (const std::exception& exception) {
        response = MakeErrorResponse(http::status::internal_server_error, "internal_error", exception.what());
    } catch (...) {
        response = MakeErrorResponse(http::status::internal_server_error, "internal_error", "unknown server error");
    }
    response.keep_alive(false);

    stream.expires_after(options_.write_timeout);

    http::write(stream, response, error);
    if (error) {
        std::cerr << "[agent] http write failed: " << error.message() << '\n';
        return;
    }
    stream.socket().shutdown(tcp::socket::shutdown_both, error);
}

void HttpServer::CloseAcceptor() noexcept {
    if (!acceptor_.is_open()) {
        return;
    }

    beast::error_code ignored_error;
    acceptor_.close(ignored_error);
}

void HttpServer::WaitNoThrow() noexcept {
    try {
        Wait();
    } catch (...) {}
}
} // namespace aegis::agent
