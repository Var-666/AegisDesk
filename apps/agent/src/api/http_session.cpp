#include "http_session.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/http.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <utility>

namespace aegis::agent {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

[[nodiscard]] bool IsExpectedSessionClose(const beast::error_code& error) noexcept {
    return error == asio::error::operation_aborted || error == asio::error::eof || error == http::error::end_of_stream
           || error == beast::error::timeout;
}

[[nodiscard]] bool IsMalformedRequest(const beast::error_code& error) noexcept {
    return error == http::error::unexpected_body || error == http::error::bad_line_ending
           || error == http::error::bad_method || error == http::error::bad_target || error == http::error::bad_version
           || error == http::error::bad_field || error == http::error::bad_value
           || error == http::error::bad_content_length || error == http::error::bad_transfer_encoding
           || error == http::error::bad_chunk || error == http::error::bad_chunk_extension
           || error == http::error::bad_obs_fold || error == http::error::multiple_content_length
           || error == http::error::header_field_name_too_large || error == http::error::header_field_value_too_large;
}

} // namespace

HttpSession::HttpSession(asio::ip::tcp::socket socket, HttpServerOptions options, HttpServer::RequestHandler handler,
                         std::shared_ptr<BoundedRequestExecutor> request_executor, CloseHandler close_handler)
    : stream_(std::move(socket))
    , io_executor_(stream_.get_executor())
    , options_(std::move(options))
    , handler_(std::move(handler))
    , request_executor_(std::move(request_executor))
    , close_handler_(std::move(close_handler)) {}

void HttpSession::Start() {
    asio::dispatch(io_executor_, [self = shared_from_this()] { self->ReadRequest(); });
}

void HttpSession::RequestStop() noexcept {
    try {
        asio::dispatch(io_executor_, [self = shared_from_this()] { self->Close(); });
    } catch (...) {
        CloseWithoutCallback();
    }
}

void HttpSession::BeginDrain() noexcept {
    drain_requested_.store(true, std::memory_order_release);

    try {
        asio::dispatch(io_executor_, [self = shared_from_this()] {
            if (self->state_ == State::kClosed) {
                return;
            }

            self->close_after_response_ = true;

            if (self->state_ == State::kReading) {
                self->Close();
            }
        });
    } catch (...) {
        CloseWithoutCallback();
    }
}

void HttpSession::ReadRequest() {
    if (state_ == State::kClosed || drain_requested_.load(std::memory_order_acquire)) {
        Close();
        return;
    }

    parser_.emplace();
    parser_->header_limit(static_cast<std::uint32_t>(options_.max_header_bytes));
    parser_->body_limit(static_cast<std::uint64_t>(options_.max_body_bytes));
    state_ = State::kReading;

    stream_.expires_after(request_count_ == 0 ? options_.read_timeout : options_.idle_timeout);
    http::async_read(stream_, buffer_, *parser_,
                     beast::bind_front_handler(&HttpSession::HandleRead, shared_from_this()));
}

void HttpSession::HandleRead(const beast::error_code& error, const std::size_t) {
    if (error) {
        if (error == http::error::header_limit) {
            request_version_ = 11;
            request_keep_alive_ = false;
            WriteResponse(MakeErrorResponse(http::status::request_header_fields_too_large, "header_too_large",
                                            "request headers exceed the configured limit"),
                          true);
            return;
        }

        if (error == http::error::body_limit) {
            request_version_ = 11;
            request_keep_alive_ = false;
            WriteResponse(MakeErrorResponse(http::status::payload_too_large, "body_too_large",
                                            "request body exceeds the configured limit"),
                          true);
            return;
        }

        if (IsMalformedRequest(error)) {
            request_version_ = 11;
            request_keep_alive_ = false;
            WriteResponse(MakeErrorResponse(http::status::bad_request, "malformed_request", error.message()), true);
            return;
        }

        if (!IsExpectedSessionClose(error)) {
            std::cerr << "[agent] http read failed: " << error.message() << '\n';
        }
        Close();
        return;
    }

    request_ = parser_->release();
    parser_.reset();
    ++request_count_;
    request_version_ = request_.version();
    request_keep_alive_ = request_.keep_alive();
    state_ = State::kHandling;

    bool submitted = false;

    try {
        submitted =
            request_executor_ != nullptr
            && request_executor_->TrySubmit([self = shared_from_this(), request = std::move(request_)]() mutable {
                   self->ExecuteRequest(std::move(request));
               });
    } catch (const std::exception& exception) {
        WriteResponse(MakeErrorResponse(http::status::internal_server_error, "dispatch_failed", exception.what()));
        return;
    } catch (...) {
        WriteResponse(MakeErrorResponse(http::status::internal_server_error, "dispatch_failed",
                                        "unknown request dispatch error"));
        return;
    }

    if (!submitted) {
        HttpResponse response = MakeErrorResponse(http::status::service_unavailable, "server_overloaded",
                                                  "too many requests are already in flight");
        response.set(http::field::retry_after, "1");
        WriteResponse(std::move(response));
    }
}

void HttpSession::ExecuteRequest(HttpRequest request) noexcept {
    HttpResponse response;

    try {
        response = handler_(request);
    } catch (const std::exception& exception) {
        response = MakeErrorResponse(http::status::internal_server_error, "internal_error", exception.what());
    } catch (...) {
        response = MakeErrorResponse(http::status::internal_server_error, "internal_error", "unknown server error");
    }

    try {
        asio::post(io_executor_, [self = shared_from_this(), response = std::move(response)]() mutable {
            self->WriteResponse(std::move(response));
        });
    } catch (...) {
        // The server shutdown path owns socket cancellation and Session reclamation.
    }
}

void HttpSession::WriteResponse(HttpResponse response, const bool force_close) {
    if (state_ == State::kClosed) {
        return;
    }

    response_ = std::move(response);
    response_.version(request_version_);
    const bool keep_alive = !force_close && !drain_requested_.load(std::memory_order_acquire) && request_keep_alive_
                            && request_count_ < options_.max_requests_per_connection;
    response_.keep_alive(keep_alive);
    close_after_response_ = !keep_alive;
    state_ = State::kWriting;
    stream_.expires_after(options_.write_timeout);
    http::async_write(stream_, response_, beast::bind_front_handler(&HttpSession::HandleWrite, shared_from_this()));
}

void HttpSession::HandleWrite(const beast::error_code& error, const std::size_t) {
    if (error) {
        if (!IsExpectedSessionClose(error)) {
            std::cerr << "[agent] http write failed: " << error.message() << '\n';
        }
        Close();
        return;
    }

    if (close_after_response_ || drain_requested_.load(std::memory_order_acquire)) {
        Close();
        return;
    }

    request_ = {};
    response_ = {};
    ReadRequest();
}

void HttpSession::Close() noexcept {
    if (state_ == State::kClosed) {
        return;
    }

    state_ = State::kClosed;
    CloseWithoutCallback();

    CloseHandler close_handler = std::move(close_handler_);
    if (close_handler) {
        close_handler(shared_from_this());
    }
}

void HttpSession::CloseWithoutCallback() noexcept {
    beast::error_code ignored_error;
    stream_.socket().cancel(ignored_error);
    stream_.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored_error);
    stream_.socket().close(ignored_error);
}

} // namespace aegis::agent
