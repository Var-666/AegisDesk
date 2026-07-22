#pragma once

#include "agent/api/http_server.h"
#include "bounded_request_executor.h"

#include <boost/asio/any_io_executor.hpp>
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
                std::shared_ptr<BoundedRequestExecutor> request_executor, CloseHandler close_handler);

    void Start();

    void RequestStop() noexcept;

private:
    void ReadRequest();

    void HandleRead(const boost::system::error_code& error, std::size_t bytes_transferred);

    void ExecuteRequest(HttpRequest request) noexcept;

    void WriteResponse(HttpResponse response);

    void HandleWrite(const boost::system::error_code& error, std::size_t bytes_transferred);

    void Close() noexcept;

    void CloseWithoutCallback() noexcept;

private:
    boost::beast::tcp_stream stream_;
    boost::asio::any_io_executor io_executor_;
    HttpServerOptions options_;
    HttpServer::RequestHandler handler_;
    std::shared_ptr<BoundedRequestExecutor> request_executor_;
    CloseHandler close_handler_;
    boost::beast::flat_buffer buffer_;
    HttpRequest request_;
    HttpResponse response_;
    bool closed_{false};
};

} // namespace aegis::agent
