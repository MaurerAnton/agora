#include "agora/core/http_policy.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/crypto_primitives.hpp"
#include "agora/core/java_uri.hpp"
#include "agora/core/json.hpp"
#include "agora/core/unicode.hpp"
#include "utf16.hpp"

namespace agora::core::http_policy {
namespace {

/** Kotlin String.lowercase() restricted to ASCII, as documented in the header. */
std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size(), suffix.size()) == suffix;
}

/** Kotlin String.trim(vararg chars): strip any of '[' and ']' from both ends. */
std::string_view trim_brackets(std::string_view text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && (text[start] == '[' || text[start] == ']')) ++start;
    while (end > start && (text[end - 1] == '[' || text[end - 1] == ']')) --end;
    return text.substr(start, end - start);
}

/** Kotlin String.split(char): trailing empty strings are kept. */
std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(delimiter, start);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

/** JDK 17 Unicode 13 Nd zero digits that fit in a UTF-16 code unit. Character.digit(char, 10)
 *  accepts these BMP decimal digits; astral Nd blocks are not accepted because Kotlin's
 *  String.toIntOrNull iterates UTF-16 chars, whose surrogates are not digits. */
constexpr std::array<char16_t, 37> kDecimalZeroDigits = {
    0x0030, 0x0660, 0x06F0, 0x07C0, 0x0966, 0x09E6, 0x0A66, 0x0AE6, 0x0B66, 0x0BE6,
    0x0C66, 0x0CE6, 0x0D66, 0x0DE6, 0x0E50, 0x0ED0, 0x0F20, 0x1040, 0x1090, 0x17E0,
    0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90, 0x1B50, 0x1BB0, 0x1C40, 0x1C50, 0xA620,
    0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0, 0xFF10,
};

int decimal_digit_value(char16_t unit) {
    for (const char16_t zero : kDecimalZeroDigits) {
        if (unit >= zero && unit < static_cast<char16_t>(zero + 10)) {
            return unit - zero;
        }
    }
    return -1;
}

/** String.toIntOrNull(): optional '+'/'-' then decimal digits, null on empty/non-digits/overflow.
 *  The digit test is Character.digit(char, 10), so BMP Unicode decimal digits count. */
std::optional<std::int64_t> parse_kotlin_int(std::string_view text) {
    const std::u16string units = utf16::from_utf8(text);
    if (units.empty()) return std::nullopt;
    std::size_t at = 0;
    bool negative = false;
    if (units[0] == u'+' || units[0] == u'-') {
        negative = units[0] == u'-';
        at = 1;
        if (at == units.size()) return std::nullopt;
    }
    std::int64_t value = 0;
    for (; at < units.size(); ++at) {
        const int digit = decimal_digit_value(units[at]);
        if (digit < 0) return std::nullopt;
        if (value > (INT64_MAX - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return negative ? -value : value;
}

bool starts_with_http_scheme(std::string_view url) {
    static constexpr std::string_view kScheme = "http://";
    if (url.size() < kScheme.size()) return false;
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kScheme[i]) return false;
    }
    return true;
}

/** java.lang.String.getBytes(ISO_8859_1): unmappable code points become '?'. */
std::vector<std::uint8_t> encode_iso_8859_1(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size());
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unicode::Decoded decoded =
            unicode::decode_utf8(text.data() + offset, text.size() - offset);
        out.push_back(decoded.code_point <= 0xFF ? static_cast<std::uint8_t>(decoded.code_point)
                                                  : std::uint8_t{0x3F});
        offset += decoded.length;
    }
    return out;
}

}  // namespace

std::optional<ProxyConfig> sanitize_proxy_config(const std::optional<ProxyConfig>& config) {
    if (!config.has_value()) return std::nullopt;
    if (json::is_blank(config->host) || config->port < 1 || config->port > 65535) {
        return std::nullopt;
    }
    return config;
}

ProxyDecision resolve_proxy(std::string_view host, const std::optional<ProxyConfig>& config) {
    if (!config.has_value()) return {};
    if (is_proxy_bypassed(host, config->bypass)) return {};
    ProxyDecision decision;
    decision.kind = config->type == ProxyType::kSocks ? ProxyKind::kSocks : ProxyKind::kHttp;
    decision.host = config->host;
    decision.port = config->port;
    return decision;
}

std::optional<std::string> proxy_authorization_header(const std::optional<ProxyConfig>& config,
                                                      bool has_existing_header) {
    if (!config.has_value() || json::is_blank(config->username) ||
        config->type != ProxyType::kHttp) {
        return std::nullopt;
    }
    if (has_existing_header) return std::nullopt;
    const std::vector<std::uint8_t> latin1 =
        encode_iso_8859_1(config->username + ":" + config->password);
    return "Basic " + crypto::base64_encode(std::span<const std::uint8_t>(latin1), false, true);
}

bool is_local_host(std::string_view host) {
    if (json::is_blank(host)) return false;
    std::string h = lower_ascii(host);
    h = std::string(trim_brackets(h));
    if (h == "localhost" || h == "::1" || ends_with(h, ".local") || ends_with(h, ".lan") ||
        ends_with(h, ".home") || ends_with(h, ".internal")) {
        return true;
    }
    if (h == "ts.net" || ends_with(h, ".ts.net")) return true;
    if (starts_with(h, "fd7a:115c:a1e0")) return true;
    if (h.find('.') == std::string::npos) return true;

    const std::vector<std::string_view> octets = split(h, '.');
    if (octets.size() == 4) {
        std::array<int, 4> values{};
        bool valid = true;
        for (std::size_t i = 0; i < octets.size(); ++i) {
            const std::optional<std::int64_t> value = parse_kotlin_int(octets[i]);
            if (!value.has_value() || *value < 0 || *value > 255) {
                valid = false;
                break;
            }
            values[i] = static_cast<int>(*value);
        }
        if (valid) {
            const int a = values[0];
            const int b = values[1];
            return a == 127 || a == 10 || (a == 192 && b == 168) ||
                   (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254) ||
                   (a == 100 && b >= 64 && b <= 127);
        }
    }
    return false;
}

void guard_cleartext_credentials(
    std::string_view url,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    if (!starts_with_http_scheme(url)) return;
    std::string host;
    if (const std::optional<java_uri::Uri> uri = java_uri::parse(url);
        uri.has_value() && uri->host.has_value()) {
        host = *uri->host;
    }
    if (is_local_host(host)) return;
    for (const auto& [name, value] : headers) {
        const std::string key = lower_ascii(name);
        if (key == "authorization" || key == "x-api-key" || key == "x-goog-api-key" ||
            key == "api-key") {
            throw std::runtime_error(
                "Refusing to send API credentials over cleartext HTTP to a non-local host. "
                "Use an https:// endpoint, or reach it over LAN/Tailscale.");
        }
    }
}

bool is_proxy_bypassed(std::string_view host, const std::vector<std::string>& bypass) {
    if (json::is_blank(host)) return true;
    const std::string h(trim_brackets(lower_ascii(host)));
    for (const std::string& raw : bypass) {
        const std::string entry = lower_ascii(json::trim(raw));
        if (entry.empty()) continue;
        if (entry.find('/') != std::string::npos) {
            if (ipv4_in_cidr(h, entry)) return true;
        } else if (starts_with(entry, "*.")) {
            if (h == entry.substr(2) || ends_with(h, entry.substr(1))) return true;
        } else if (h == entry) {
            return true;
        }
    }
    return false;
}

std::optional<std::uint32_t> ipv4_to_long(std::string_view ip) {
    const std::vector<std::string_view> octets = split(ip, '.');
    if (octets.size() != 4) return std::nullopt;
    std::uint32_t value = 0;
    for (const std::string_view octet : octets) {
        const std::optional<std::int64_t> parsed = parse_kotlin_int(octet);
        if (!parsed.has_value() || *parsed < 0 || *parsed > 255) return std::nullopt;
        value = (value << 8) | static_cast<std::uint32_t>(*parsed);
    }
    return value;
}

bool ipv4_in_cidr(std::string_view host, std::string_view cidr) {
    const std::vector<std::string_view> parts = split(cidr, '/');
    if (parts.size() != 2) return false;
    const std::optional<std::int64_t> bits = parse_kotlin_int(parts[1]);
    if (!bits.has_value() || *bits < 0 || *bits > 32) return false;
    const std::optional<std::uint32_t> address = ipv4_to_long(host);
    const std::optional<std::uint32_t> network = ipv4_to_long(parts[0]);
    if (!address.has_value() || !network.has_value()) return false;
    const std::uint32_t mask =
        *bits == 0 ? 0u : (0xFFFFFFFFu << static_cast<unsigned>(32 - *bits));
    return (*address & mask) == (*network & mask);
}

ClientPolicy client_policy() { return ClientPolicy{}; }

}  // namespace agora::core::http_policy
