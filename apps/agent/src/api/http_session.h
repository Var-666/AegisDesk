#pragma once

#include "agent/api/http_server.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#include <functional>
#include <memory>

namespace aegis::agent {

class HttpSession final : public std::enable_shared_from_this<HttpSession> {
public:
    using CloseHandler = std::function<void(const std::shared_ptr<HttpSession>&)>;

    HttpSession(boost::asio::ip::tcp::socket socket, HttpServerOptions options, HttpServer::RequestHandler handler,
                CloseHandler close_handler);

    void Start();

    void RequestStop() noexcept;

private:
    void ReadRequest();

    void HandleRead(const boost::system::error_code& error, std::size_t bytes_transferred);

    void HandleWrite(const boost::system::error_code& error, std::size_t bytes_transferred);

    void Close() noexcept;

    void CloseWithoutCallback() noexcept;

private:
    boost::beast::tcp_stream stream_;
    HttpServerOptions options_;
    HttpServer::RequestHandler handler_;
    CloseHandler close_handler_;
    boost::beast::flat_buffer buffer_;
    HttpRequest request_;
    HttpResponse response_;
    bool closed_{false};
};

} // namespace aegis::agent
