#include "agora/core/conch_network.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace agora::core::conch {
namespace {

bool iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

void set_header(std::vector<Header>& headers, std::string name, std::string value) {
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [&](const Header& header) {
                                     return iequals(header.name, name);
                                 }),
                  headers.end());
    headers.push_back(Header{std::move(name), std::move(value)});
}

TextResponse execute(Transport& transport, const Request& request,
                     std::int64_t success_limit) {
    RawResponse raw = transport.execute(request);
    const bool is_successful = raw.code >= 200 && raw.code <= 299;

    std::string signature;
    for (const Header& header : raw.headers) {
        if (iequals(header.name, "X-Conch-Response-Signature")) signature = header.value;
    }

    std::string body =
        wire::read_bounded_wire_text(*raw.body, is_successful ? success_limit : CONTROL_LIMIT);
    return TextResponse{raw.code, std::move(body), is_successful, std::move(signature)};
}

}  // namespace

ClientConfig client_config() { return ClientConfig{}; }

Request get_text_request(std::string_view url, std::vector<Header> headers) {
    Request request;
    request.method = "GET";
    request.url = std::string(url);
    for (Header& header : headers) {
        set_header(request.headers, std::move(header.name), std::move(header.value));
    }
    request.timeout_millis = 30'000;
    return request;
}

Request post_text_request(std::string_view url, std::string_view body,
                          std::vector<Header> headers,
                          std::optional<std::int64_t> call_timeout_millis) {
    const std::int64_t timeout_millis = call_timeout_millis.value_or(130'000);
    if (!(timeout_millis > 0)) throw std::invalid_argument("Failed requirement.");

    Request request;
    request.method = "POST";
    request.url = std::string(url);
    for (Header& header : headers) {
        set_header(request.headers, std::move(header.name), std::move(header.value));
    }
    request.body = std::string(body);
    request.content_type = "application/json";
    request.timeout_millis = timeout_millis;
    return request;
}

TextResponse get_text_response(Transport& transport, std::string_view url,
                               std::vector<Header> headers) {
    return execute(transport, get_text_request(url, std::move(headers)), CONTROL_LIMIT);
}

TextResponse post_text_response(Transport& transport, std::string_view url, std::string_view body,
                                std::vector<Header> headers,
                                std::optional<std::int64_t> call_timeout_millis) {
    return execute(transport,
                   post_text_request(url, body, std::move(headers), call_timeout_millis),
                   RESPONSE_LIMIT);
}

}  // namespace agora::core::conch
