#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/conch_network.hpp"
#include "agora/core/http_policy.hpp"
#include "agora/core/shell_tool_backends.hpp"

namespace agora::core {

#ifdef AGORA_CORE_HAVE_CURL

/**
 * Configuration for the CurlTransport.
 */
struct CurlTransportConfig {
    std::int32_t default_timeout_ms = 30'000;
    bool follow_redirects = false;
    bool follow_ssl_redirects = false;
    bool retry_on_connection_failure = false;
};

/**
 * Curl-based HTTP transport implementing both ConchNetwork.Transport and
 * shell_backends::HttpStreamTransport.
 *
 * Uses libcurl's multi interface for the streaming handle (HttpStreamHandle) and
 * easy interface for the simple requests (ConchTransport).
 *
 * Policy surface (installed by the app layer and passed at call sites):
 *   - proxy resolution (resolve_proxy)
 *   - cleartext credential guard (guard_cleartext_credentials)
 *   - proxy authorization (proxy_authorization_header)
 *   - follow_redirects / follow_ssl_redirects / retry_on_connection_failure
 *   - pool settings (kept by the app; here we just configure the easy handles)
 *
 * TLS verification uses the system trust store (CURLOPT_SSL_VERIFYPEER=1).
 * All timeouts are in milliseconds; 0 means libcurl default (no timeout).
 */
class CurlTransport final
    : public conch::Transport,
      public shell_backends::HttpStreamTransport {
public:
    using Config = CurlTransportConfig;

    struct StreamHandle;

    explicit CurlTransport(Config config = CurlTransportConfig{});
    ~CurlTransport() override;

    CurlTransport(const CurlTransport&) = delete;
    CurlTransport& operator=(const CurlTransport&) = delete;

    // ── conch::Transport ───────────────────────────────────────────────────────
    conch::RawResponse execute(const conch::Request& request) override;

    // ── shell_backends::HttpStreamTransport ────────────────────────────────────
    std::unique_ptr<shell_backends::HttpStreamHandle>
    stream_post(const shell_backends::StreamCall& call) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Install the process-wide proxy selector for libcurl.
 * Mirrors HttpClient.installProxy: sets CURLOPT_PROXY, CURLOPT_PROXYPORT,
 * CURLOPT_PROXYTYPE, CURLOPT_PROXYUSERPWD, CURLOPT_NOPROXY (bypass),
 * CURLOPT_PROXYAUTH (BASIC).
 */
void install_curl_proxy(const std::optional<http_policy::ProxyConfig>& config);

/**
 * Guard cleartext credentials per http_policy.hpp (throws std::runtime_error
 * on violation). Called before every request.
 */
void guard_curl_cleartext_credentials(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers);

#endif  // AGORA_CORE_HAVE_CURL

}  // namespace agora::core