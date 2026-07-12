#pragma once

#include <boost/beast/http.hpp>

#include <string>
#include <string_view>

namespace aegis::agent {

using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

std::string JsonEscape(std::string_view value);

HttpResponse MakeJsonResponse(boost::beast::http::status status, std::string body);

HttpResponse MakeErrorResponse(boost::beast::http::status status, std::string_view error, std::string_view message);

} // namespace aegis::agent
