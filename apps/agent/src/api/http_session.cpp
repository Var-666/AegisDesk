#include "http_session.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/http.hpp>

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

void HttpSession::ReadRequest() {
    if (closed_) {
        return;
    }

    stream_.expires_after(options_.read_timeout);
    http::async_read(stream_, buffer_, request_,
                     beast::bind_front_handler(&HttpSession::HandleRead, shared_from_this()));
}

void HttpSession::HandleRead(const beast::error_code& error, const std::size_t) {
    if (error) {
        if (!IsExpectedSessionClose(error)) {
            std::cerr << "[agent] http read failed: " << error.message() << '\n';
        }
        Close();
        return;
    }

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

void HttpSession::WriteResponse(HttpResponse response) {
    if (closed_) {
        return;
    }

    response_ = std::move(response);
    response_.keep_alive(false);
    stream_.expires_after(options_.write_timeout);
    http::async_write(stream_, response_, beast::bind_front_handler(&HttpSession::HandleWrite, shared_from_this()));
}

void HttpSession::HandleWrite(const beast::error_code& error, const std::size_t) {
    if (error && !IsExpectedSessionClose(error)) {
        std::cerr << "[agent] http write failed: " << error.message() << '\n';
    }

    Close();
}

void HttpSession::Close() noexcept {
    if (closed_) {
        return;
    }

    closed_ = true;
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
