//
// Created by Var on 2026/7/3.
//

#include "../include/agent/http_server.h"

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <iostream>
#include <thread>

namespace aegis::agent {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

} // namespace

HttpServer::HttpServer(const HttpServerOptions& options, RequestHandler handler)
    : options_(std::move(options))
    , handler_(std::move(handler)) {}

void HttpServer::Run(const std::function<bool()>& stop_requested) {
    beast::error_code error;

    const auto address = asio::ip::make_address(options_.bind_address, error);
    if (error) {
        throw std::runtime_error("invalid bind address: " + error.message());
    }

    tcp::acceptor acceptor(io_context_);

    acceptor.open(tcp::v4(), error);
    if (error) {
        throw std::runtime_error("acceptor open failed: " + error.message());
    }

    acceptor.set_option(asio::socket_base::reuse_address(true), error);
    if (error) {
        throw std::runtime_error("acceptor set_option failed: " + error.message());
    }

    acceptor.bind({address, options_.port}, error);
    if (error) {
        throw std::runtime_error("acceptor bind failed: " + error.message());
    }

    acceptor.listen(asio::socket_base::max_listen_connections, error);
    if (error) {
        throw std::runtime_error("acceptor listen failed: " + error.message());
    }

    acceptor.non_blocking(true, error);
    if (error) {
        throw std::runtime_error("acceptor non_blocking failed: " + error.message());
    }

    while (!stop_requested()) {
        tcp::socket socket(io_context_);

        acceptor.accept(socket, error);

        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (error) {
            std::cerr << "[agent] accept failed: " << error.message() << '\n';
            continue;
        }

        HandleSession(std::move(socket));
    }
}
void HttpServer::HandleSession(boost::asio::ip::tcp::socket socket) {
    beast::tcp_stream stream(std::move(socket));

    stream.expires_after(std::chrono::seconds(2));

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

    stream.expires_after(std::chrono::seconds(15));

    http::write(stream, response, error);
    if (error) {
        std::cerr << "[agent] http write failed: " << error.message() << '\n';
        return;
    }
    stream.socket().shutdown(tcp::socket::shutdown_both, error);
}
} // namespace aegis::agent