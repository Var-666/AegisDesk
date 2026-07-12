//
// Created by Var on 2026/7/3.
//

#include "agent/api/http_json.h"

#include <iomanip>
#include <sstream>

namespace aegis::agent {
std::string JsonEscape(std::string_view value) {
    std::ostringstream output;

    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    output << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(character)
                           << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(character);
                }
                break;
        }
    }

    return output.str();
}

HttpResponse MakeJsonResponse(boost::beast::http::status status, std::string body) {
    namespace http = boost::beast::http;

    HttpResponse response{status, 11};

    response.set(http::field::content_type, "application/json; charset=utf-8");
    response.set(http::field::cache_control, "no-store");

    response.body() = std::move(body);
    response.prepare_payload();

    return response;
}
HttpResponse MakeErrorResponse(boost::beast::http::status status, std::string_view error, std::string_view message) {
    return MakeJsonResponse(status,
                            R"({"error":")" + JsonEscape(error) + R"(","message":")" + JsonEscape(message) + "\"}");
}
} // namespace aegis::agent