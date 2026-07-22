#include "http_session.h"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
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
                         CloseHandler close_handler)
    : stream_(std::move(socket))
    , options_(std::move(options))
    , handler_(std::move(handler))
    , close_handler_(std::move(close_handler)) {}

void HttpSession::Start() {
    asio::dispatch(stream_.get_executor(), [self = shared_from_this()] { self->ReadRequest(); });
}

void HttpSession::RequestStop() noexcept {
    try {
        asio::dispatch(stream_.get_executor(), [self = shared_from_this()] { self->Close(); });
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

    try {
        response_ = handler_(request_);
    } catch (const std::exception& exception) {
        response_ = MakeErrorResponse(http::status::internal_server_error, "internal_error", exception.what());
    } catch (...) {
        response_ = MakeErrorResponse(http::status::internal_server_error, "internal_error", "unknown server error");
    }

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
