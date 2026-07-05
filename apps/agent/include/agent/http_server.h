#pragma once

#include "agent/http_json.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <functional>
#include <string>

namespace aegis::agent {
struct HttpServerOptions {
    std::string bind_address{"127.0.0.1"};
    unsigned short port{18081};
};

class HttpServer {
public:
    using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer(const HttpServerOptions& options, RequestHandler handler);

    void Run(const std::function<bool()>& stop_requested);

private:
    void HandleSession(boost::asio::ip::tcp::socket socket);

private:
    HttpServerOptions options_;
    RequestHandler handler_;
    boost::asio::io_context io_context_{-1};
};

} // namespace aegis::agent
