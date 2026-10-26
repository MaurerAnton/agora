#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/bounded_wire_read.hpp"

namespace agora::core::conch {

// ConchNetwork.kt limits.
inline constexpr std::int64_t CONTROL_LIMIT = 64LL << 10;
inline constexpr std::int64_t RESPONSE_LIMIT = 40LL << 20;
inline constexpr std::int64_t LINE_LIMIT = 10LL << 20;

// ConchNetwork.kt POOL_KEEP_ALIVE_SECONDS.
inline constexpr std::int64_t POOL_KEEP_ALIVE_SECONDS = 45;

struct Header {
    std::string name;
    std::string value;
};

/**
 * The four properties ConchNetwork overrides on HttpClient.client, plus the pool it installs.
 * The transport owns the actual OkHttpClient; this pins the policy it must be built with.
 */
struct ClientConfig {
    bool retry_on_connection_failure = false;
    bool follow_redirects = false;
    bool follow_ssl_redirects = false;
    int pool_max_idle_connections = 5;
    std::int64_t pool_keep_alive_seconds = POOL_KEEP_ALIVE_SECONDS;
};

ClientConfig client_config();

/**
 * One Conch request after header replacement. `content_type` is empty for GET and
 * "application/json" for POST, matching `body.toRequestBody("application/json".toMediaType())`.
 * `timeout_millis` is the per-call timeout applied through `Call.timeout()`.
 */
struct Request {
    std::string method;
    std::string url;
    std::vector<Header> headers;
    std::optional<std::string> body;
    std::string content_type;
    std::int64_t timeout_millis = 0;
};

/** `Request.Builder().url(url).get()` followed by the caller's headers. */
Request get_text_request(std::string_view url, std::vector<Header> headers);

/** `Request.Builder().url(url).post(body.toRequestBody("application/json".toMediaType()))`. */
Request post_text_request(std::string_view url, std::string_view body,
                          std::vector<Header> headers,
                          std::optional<std::int64_t> call_timeout_millis = std::nullopt);

struct TextResponse {
    int code = 0;
    std::string body;
    bool is_successful = false;
    std::string signature;
};

/**
 * The platform half: performs one call and returns the response body as a wire::Source.
 * The source's destructor must close the underlying response, mirroring `Response.use`.
 */
struct RawResponse {
    int code = 0;
    std::vector<Header> headers;
    std::unique_ptr<wire::Source> body;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual RawResponse execute(const Request& request) = 0;
};

TextResponse get_text_response(Transport& transport, std::string_view url,
                               std::vector<Header> headers);

TextResponse post_text_response(Transport& transport, std::string_view url, std::string_view body,
                                std::vector<Header> headers,
                                std::optional<std::int64_t> call_timeout_millis = std::nullopt);

}  // namespace agora::core::conch
