#include "agora/core/diagnostic_redactor.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::diagnostic {

namespace {

// ---- Minimal HttpUrl parser ---------------------------------------------------------

struct HttpUrlImpl {
    std::string scheme;
    std::string username;
    std::string password;
    std::string host;
    int port = -1;
    std::string path;
    std::vector<std::pair<std::string, std::string>> query_params;
    std::string fragment;
    bool valid = false;

    std::string to_string() const {
        std::string out = scheme + "://";
        if (!username.empty()) {
            out += username;
            if (!password.empty()) out += ":" + password;
            out += "@";
        }
        out += host;
        if (port != -1) out += ":" + std::to_string(port);
        out += path;
        if (!query_params.empty()) {
            out += "?";
            for (size_t i = 0; i < query_params.size(); ++i) {
                if (i) out += "&";
                out += query_params[i].first + "=" + query_params[i].second;
            }
        }
        if (!fragment.empty()) out += "#" + fragment;
        return out;
    }

    static std::optional<HttpUrlImpl> parse(std::string_view raw) {
        HttpUrlImpl out;
        std::string s(raw);

        // scheme
        size_t scheme_end = s.find("://");
        if (scheme_end == std::string::npos) return std::nullopt;
        out.scheme = s.substr(0, scheme_end);
        s = s.substr(scheme_end + 3);

        // userinfo
        size_t at = s.find('@');
        if (at != std::string::npos) {
            std::string userinfo = s.substr(0, at);
            size_t colon = userinfo.find(':');
            if (colon != std::string::npos) {
                out.username = userinfo.substr(0, colon);
                out.password = userinfo.substr(colon + 1);
            } else {
                out.username = userinfo;
            }
            s = s.substr(at + 1);
        }

        // host:port/path?query#fragment
        size_t path_start = s.find('/');
        size_t query_start = s.find('?');
        size_t fragment_start = s.find('#');

        size_t authority_end = std::string::npos;
        for (size_t pos : {path_start, query_start, fragment_start}) {
            if (pos != std::string::npos &&
                (authority_end == std::string::npos || pos < authority_end)) {
                authority_end = pos;
            }
        }

        std::string authority = (authority_end == std::string::npos) ? s : s.substr(0, authority_end);
        std::string rest = (authority_end == std::string::npos) ? "" : s.substr(authority_end);

        size_t colon = authority.find(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            out.port = std::stoi(authority.substr(colon + 1));
        } else {
            out.host = authority;
        }

        // parse rest
        if (path_start != std::string::npos) {
            size_t end = std::string::npos;
            for (size_t pos : {query_start, fragment_start}) {
                if (pos != std::string::npos && (end == std::string::npos || pos < end)) {
                    end = pos;
                }
            }
            out.path = (end == std::string::npos) ? rest : rest.substr(0, end - path_start);
        }

        if (query_start != std::string::npos) {
            size_t end = fragment_start;
            std::string query = (end == std::string::npos)
                ? s.substr(query_start + 1)
                : s.substr(query_start + 1, end - query_start - 1);
            size_t pos = 0;
            while (pos < query.size()) {
                size_t amp = query.find('&', pos);
                std::string pair = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
                size_t eq = pair.find('=');
                if (eq != std::string::npos) {
                    out.query_params.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
                }
                pos = (amp == std::string::npos) ? query.size() : amp + 1;
            }
        }

        if (fragment_start != std::string::npos) {
            out.fragment = s.substr(fragment_start + 1);
        }

        out.valid = true;
        return out;
    }
};

}  // namespace

// ---- DiagnosticRedactor implementation ----------------------------------------------

DiagnosticRedactor::DiagnosticRedactor(std::size_t max_payload_bytes)
    : max_payload_bytes_(max_payload_bytes) {
    // json_ is default-constructed; our Json::parse handles ignoreUnknownKeys internally
}

// --- Public capture API --------------------------------------------------------------

CapturedDiagnosticText DiagnosticRedactor::capture_url(std::string_view raw_url) {
    auto parsed = HttpUrlImpl::parse(raw_url);
    std::string sanitized;
    if (!parsed) {
        sanitized = INVALID_URL;
    } else {
        auto builder = *parsed;
        if (!builder.username.empty()) builder.username = REDACTED_SECRET;
        if (!builder.password.empty()) builder.password = REDACTED_SECRET;
        for (auto& qp : builder.query_params) {
            if (is_secret_key(qp.first)) qp.second = REDACTED_SECRET;
        }
        sanitized = redact_request_metadata(builder.to_string());
    }
    return capture(raw_url, sanitized);
}

std::vector<std::pair<std::string, std::string>> DiagnosticRedactor::capture_headers(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(std::min(headers.size(), MAX_HEADERS));
    for (size_t i = 0; i < headers.size() && i < MAX_HEADERS; ++i) {
        std::string name = headers[i].first;
        std::string value = headers[i].second;
        if (name.size() > MAX_IDENTIFIER_CHARS) name = name.substr(0, MAX_IDENTIFIER_CHARS);
        if (is_secret_key(name)) {
            value = REDACTED_SECRET;
        } else {
            value = redact_request_metadata(value);
        }
        out.emplace_back(std::move(name), std::move(value));
    }
    if (headers.size() > MAX_HEADERS) {
        out.emplace_back("[TRUNCATED_HEADERS]", std::to_string(headers.size() - MAX_HEADERS));
    }
    return out;
}

CapturedDiagnosticText DiagnosticRedactor::capture_json(
    std::string_view raw_json, const std::vector<std::string>& credential_values) {
    auto [input, input_truncated] = bounded_input(raw_json, max_payload_bytes_);
    std::string sanitized = credential_text(input, credential_values);
    return capture(raw_json, sanitized, input_truncated);
}

CapturedDiagnosticText DiagnosticRedactor::capture_wire_line(std::string_view raw_line) {
    auto [input, input_truncated] = bounded_input(raw_line, max_payload_bytes_);
    std::string trimmed = input;
    // trim start
    size_t start = 0;
    while (start < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[start]))) ++start;
    std::string leading = trimmed.substr(0, start);
    trimmed = trimmed.substr(start);

    std::string sanitized;
    if (trimmed.rfind("data:", 0) == 0) {
        std::string data = trimmed.substr(5);
        // trim start of data
        size_t ds = 0;
        while (ds < data.size() && std::isspace(static_cast<unsigned char>(data[ds]))) ++ds;
        data = data.substr(ds);
        if (data == "[DONE]") {
            sanitized = leading + "data: [DONE]";
        } else {
            sanitized = leading + "data: " + credential_text(data, {});
        }
    } else if (trimmed.rfind("{", 0) == 0 || trimmed.rfind("[", 0) == 0) {
        sanitized = leading + credential_text(trimmed, {});
    } else if (is_sse_control(trimmed)) {
        sanitized = input;  // keep control lines intact
    } else {
        sanitized = redact_credential_values(input);
    }
    return capture(raw_line, sanitized, input_truncated);
}

CapturedDiagnosticText DiagnosticRedactor::capture_content(std::string_view content) {
    auto [input, input_truncated] = bounded_input(content, max_payload_bytes_);
    std::string sanitized = redact_credential_values(input);
    return capture(content, sanitized, input_truncated);
}

// --- Export projections --------------------------------------------------------------

CapturedDiagnosticText DiagnosticRedactor::redact_json_content(const CapturedDiagnosticText& captured) {
    return project(captured, credential_text(captured.value, {}));
}

CapturedDiagnosticText DiagnosticRedactor::redact_wire_content(const CapturedDiagnosticText& captured) {
    std::string raw_line = captured.value;
    size_t start = 0;
    while (start < raw_line.size() && std::isspace(static_cast<unsigned char>(raw_line[start]))) ++start;
    std::string leading = raw_line.substr(0, start);
    std::string trimmed = raw_line.substr(start);

    std::string redacted;
    if (trimmed.rfind("data:", 0) == 0) {
        std::string data = trimmed.substr(5);
        size_t ds = 0;
        while (ds < data.size() && std::isspace(static_cast<unsigned char>(data[ds]))) ++ds;
        data = data.substr(ds);
        if (data == "[DONE]") {
            redacted = leading + "data: [DONE]";
        } else {
            redacted = leading + "data: " + credential_text(data, {});
        }
    } else if (trimmed.rfind("{", 0) == 0 || trimmed.rfind("[", 0) == 0) {
        redacted = leading + credential_text(trimmed, {});
    } else if (is_sse_control(trimmed)) {
        redacted = raw_line;
    } else {
        redacted = redact_credential_values(raw_line);
    }
    return project(captured, redacted);
}

CapturedDiagnosticText DiagnosticRedactor::redact_content(const CapturedDiagnosticText& captured) {
    return project(captured, redact_credential_values(captured.value));
}

std::string DiagnosticRedactor::safe_identifier(std::string_view value) {
    return redact_request_metadata(std::string(value)).substr(0, MAX_IDENTIFIER_CHARS);
}

// --- Internal helpers ----------------------------------------------------------------

bool DiagnosticRedactor::is_secret_key(std::string_view key) {
    std::string normalized = normalize_key(key);
    const auto& keys = secret_keys();
    const auto& suffixes = secret_key_suffixes();
    if (std::find(keys.begin(), keys.end(), normalized) != keys.end()) return true;
    if (normalized.rfind(CONCH_KEY_PREFIX, 0) == 0) return true;
    for (const auto& suf : suffixes) {
        if (normalized.size() >= suf.size() &&
            normalized.compare(normalized.size() - suf.size(), suf.size(), suf) == 0) {
            return true;
        }
    }
    return false;
}

std::string DiagnosticRedactor::normalize_key(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(std::tolower(c));
    }
    return out;
}

std::string DiagnosticRedactor::redact_credential_values(std::string_view value) {
    std::string str(value);
    static const std::regex BEARER_SECRET(R"(\b(Bearer\s+)[A-Za-z0-9._~+/=-]+)", std::regex_constants::icase);
    str = std::regex_replace(str, BEARER_SECRET, "$1" + std::string(REDACTED_SECRET));

    // Private key patterns (also handle in credential values for completeness)
    static const std::regex PRIVATE_KEY_BLOCK(
        R"(-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z0-9 ]*PRIVATE KEY-----)",
        std::regex_constants::icase);
    str = std::regex_replace(str, PRIVATE_KEY_BLOCK, REDACTED_SECRET);

    static const std::regex PRIVATE_KEY_PREFIX(
        R"(-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----[\s\S]*)",
        std::regex_constants::icase);
    str = std::regex_replace(str, PRIVATE_KEY_PREFIX, REDACTED_SECRET);

    static const std::vector<std::regex> SECRET_TOKEN_PATTERNS = {
        std::regex(R"(\bsk-[A-Za-z0-9_-]{12,}\b)"),
        std::regex(R"(\bAIza[A-Za-z0-9_-]{20,}\b)"),
        std::regex(R"(\bgh[pousr]_[A-Za-z0-9]{20,}\b)"),
        std::regex(R"(\bxox[baprs]-[A-Za-z0-9-]{12,}\b)"),
        std::regex(R"(\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\b)"),
    };
    for (const auto& pat : SECRET_TOKEN_PATTERNS) {
        str = std::regex_replace(str, pat, REDACTED_SECRET);
    }
    return str;
}

std::string DiagnosticRedactor::redact_request_metadata(std::string_view value) {
    std::string str(value);
    static const std::regex PRIVATE_KEY_BLOCK(
        R"(-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z0-9 ]*PRIVATE KEY-----)",
        std::regex_constants::icase);
    str = std::regex_replace(str, PRIVATE_KEY_BLOCK, REDACTED_SECRET);

    static const std::regex PRIVATE_KEY_PREFIX(
        R"(-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----[\s\S]*)",
        std::regex_constants::icase | std::regex_constants::ECMAScript);
    str = std::regex_replace(str, PRIVATE_KEY_PREFIX, REDACTED_SECRET);

    static const std::regex NAMED_SECRET(
        R"(\b(api[_-]?key|access[_-]?token|refresh[_-]?token|security[_-]?token|session[_-]?token|authorization|proxy[_-]?authorization|cookie|password|secret|signature|credential|token)\s*([=:])\s*["']?[^\s"'&,}]+)",
        std::regex_constants::icase);
    str = std::regex_replace(str, NAMED_SECRET, "$1$2" + std::string(REDACTED_SECRET));

    return redact_credential_values(str);
}

std::string DiagnosticRedactor::redact_known_credentials(
    std::string value, const std::vector<std::string>& credential_values) {
    std::vector<std::string> sorted = credential_values;
    std::sort(sorted.begin(), sorted.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& cred : sorted) {
        if (!cred.empty()) {
            size_t pos = 0;
            while ((pos = value.find(cred, pos)) != std::string::npos) {
                value.replace(pos, cred.size(), REDACTED_SECRET);
                pos += strlen(REDACTED_SECRET);
            }
        }
    }
    return value;
}

std::string DiagnosticRedactor::credential_text(
    std::string_view raw_json, const std::vector<std::string>& credential_values) {
    json::ParseResult parsed = json::parse(raw_json);
    if (!parsed.value) return redact_known_credentials(redact_credential_values(raw_json), credential_values);
    json::Value sanitized = sanitize_credential_element(*parsed.value, credential_values);
    return json::to_kotlin_string(sanitized);
}

json::Value DiagnosticRedactor::sanitize_credential_element(
    const json::Value& element, const std::vector<std::string>& credential_values) {
    if (element.kind == json::Kind::Object) {
        json::Value out;
        out.kind = json::Kind::Object;
        for (const json::Member& member : element.members) {
            json::Value sanitized = sanitize_credential_element(member.value, credential_values);
            out.members.push_back(json::Member{member.key, std::move(sanitized)});
        }
        return out;
    }
    if (element.kind == json::Kind::Array) {
        json::Value out;
        out.kind = json::Kind::Array;
        for (const json::Value& child : element.elements) {
            out.elements.push_back(sanitize_credential_element(child, credential_values));
        }
        return out;
    }
    if (element.kind == json::Kind::String || element.kind == json::Kind::Literal) {
        std::string sanitized = redact_known_credentials(
            redact_credential_values(element.text), credential_values);
        json::Value out;
        out.kind = element.kind;
        out.text = std::move(sanitized);
        return out;
    }
    return element;
}

CapturedDiagnosticText DiagnosticRedactor::capture(
    std::string_view original, std::string_view sanitized, bool already_truncated) {
    bool oversized = sanitized.size() > max_payload_bytes_;
    std::string final_value;
    if (oversized) {
        final_value = decode_utf8_prefix(sanitized, max_payload_bytes_);
    } else {
        final_value = sanitized;
    }
    return CapturedDiagnosticText{
        std::move(final_value),
        original.size(),
        already_truncated || oversized,
        true
    };
}

CapturedDiagnosticText DiagnosticRedactor::project(
    const CapturedDiagnosticText& original, std::string_view sanitized) {
    return capture(original.value, sanitized, original.truncated);
}

std::pair<std::string, bool> DiagnosticRedactor::bounded_input(std::string_view value, std::size_t max_bytes) {
    if (value.size() > max_bytes) {
        return {decode_utf8_prefix(value, max_bytes), true};
    }
    return {std::string(value), false};
}

std::string DiagnosticRedactor::decode_utf8_prefix(std::string_view bytes, std::size_t max_bytes) {
    // Simple UTF-8 prefix decode: walk bytes until we hit max_bytes, ignore incomplete sequences
    std::string out;
    out.reserve(max_bytes);
    size_t i = 0;
    while (i < bytes.size() && out.size() < max_bytes) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        size_t seq_len = 1;
        if ((c & 0x80) == 0x00) seq_len = 1;
        else if ((c & 0xE0) == 0xC0) seq_len = 2;
        else if ((c & 0xF0) == 0xE0) seq_len = 3;
        else if ((c & 0xF8) == 0xF0) seq_len = 4;
        if (i + seq_len > bytes.size()) break;  // incomplete sequence
        out.append(bytes.substr(i, seq_len));
        i += seq_len;
    }
    return out;
}

bool DiagnosticRedactor::is_sse_control(std::string_view trimmed) {
    return trimmed.empty() || trimmed[0] == ':' || trimmed.rfind("event:", 0) == 0 ||
           trimmed.rfind("id:", 0) == 0 || trimmed.rfind("retry:", 0) == 0;
}

const std::vector<std::string>& DiagnosticRedactor::secret_keys() {
    static const std::vector<std::string> keys = {
        "authorization", "proxyauthorization", "apikey", "xapikey", "xgoogapikey",
        "cookie", "setcookie", "key", "password", "passwd", "secret",
        "clientsecret", "accesstoken", "refreshtoken", "securitytoken",
        "sessiontoken", "token", "signature", "sig", "credential",
        "xamzcredential", "xamzsignature", "xamzsecuritytoken",
        "xgoogcredential", "xgoogsignature", "googleaccessid",
        "awsaccesskeyid", "proxyusername", "proxypassword",
    };
    return keys;
}

const std::vector<std::string>& DiagnosticRedactor::secret_key_suffixes() {
    static const std::vector<std::string> suffixes = {
        "apikey", "accesstoken", "refreshtoken", "clientsecret",
        "securitytoken", "sessiontoken", "signature", "credential",
    };
    return suffixes;
}

}  // namespace agora::core::diagnostic