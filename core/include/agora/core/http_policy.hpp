#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Transport-neutral policy from api/HttpClient.kt: host locality, the cleartext credential
// guard, proxy sanitisation/bypass/CIDR matching and proxy authorization. OkHttp itself (sockets,
// TLS, calls, streaming, diagnostics) stays on the platform side; the app adapter applies this
// policy when it builds/selects a client. The golden stand `core/tools/golden/http-policy` pins
// the Kotlin declarations extracted verbatim from HttpClient.kt.
//
// Boundary: Kotlin `String.lowercase()` is Unicode-aware; this port folds ASCII only, matching
// the rest of core (hosts and header names are ASCII in the pinned corpus).
namespace agora::core::http_policy {

enum class ProxyType { kHttp, kSocks };

struct ProxyConfig {
    ProxyType type = ProxyType::kHttp;
    std::string host;
    int port = 0;
    std::string username;
    std::string password;
    std::vector<std::string> bypass;
};

/** setProxy's takeIf: a null config, blank host or port outside 1..65535 clears the proxy. */
std::optional<ProxyConfig> sanitize_proxy_config(const std::optional<ProxyConfig>& config);

enum class ProxyKind { kDirect, kHttp, kSocks };

struct ProxyDecision {
    ProxyKind kind = ProxyKind::kDirect;
    std::string host;
    int port = 0;
};

/** resolveProxy: direct when there is no config or the host matches a bypass entry, otherwise
 *  the configured HTTP/SOCKS proxy. The caller passes the sanitized config, as setProxy stores
 *  it. */
ProxyDecision resolve_proxy(std::string_view host, const std::optional<ProxyConfig>& config);

/** proxyAuthenticator + OkHttp Credentials.basic: HTTP proxy only, non-blank username, and only
 *  when the request does not already carry a Proxy-Authorization header. ISO-8859-1 + Base64. */
std::optional<std::string> proxy_authorization_header(const std::optional<ProxyConfig>& config,
                                                      bool has_existing_header);

/** True for loopback / RFC-1918 / link-local hosts, bare LAN hostnames, Tailscale MagicDNS and
 *  the Tailscale ULA prefix. Public FQDNs and public IPs return false. */
bool is_local_host(std::string_view host);

/** Fail-closed guard: throws std::runtime_error (the Kotlin IOException) when an http:// URL
 *  targets a non-local host and any header name carries an API credential. */
void guard_cleartext_credentials(
    std::string_view url,
    const std::vector<std::pair<std::string, std::string>>& headers);

/** True if host matches an exact entry, a `*.suffix` wildcard or an IPv4 CIDR. */
bool is_proxy_bypassed(std::string_view host, const std::vector<std::string>& bypass);

std::optional<std::uint32_t> ipv4_to_long(std::string_view ip);
bool ipv4_in_cidr(std::string_view host, std::string_view cidr);

struct ClientPolicy {
    std::int64_t connect_timeout_seconds = 30;
    std::int64_t read_timeout_seconds = 300;
    std::int64_t write_timeout_seconds = 30;
    bool retry_on_connection_failure = true;
    bool follow_redirects = true;
    bool follow_ssl_redirects = true;
};

/** HttpClient.client's builder: 30s connect, 5min read, 30s write, OkHttp 5 retry/redirect
 *  defaults. */
ClientPolicy client_policy();

}  // namespace agora::core::http_policy
