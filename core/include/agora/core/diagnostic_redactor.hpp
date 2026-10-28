#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::diagnostic {

/** Result of a redaction operation. */
struct CapturedDiagnosticText {
    std::string value;         // the sanitized text
    std::size_t original_length = 0;
    bool truncated = false;
    bool redacted = false;
};

/** DiagnosticRedactor — permanently removes credentials from diagnostic payloads.
 *
 * Port of diagnostics/DiagnosticRedactor.kt. Uses our JSON library (json.hpp)
 * and a minimal HttpUrl parser instead of kotlinx.serialization + okhttp3.
 */
class DiagnosticRedactor {
public:
    explicit DiagnosticRedactor(std::size_t max_payload_bytes = 1'000'000);

    // --- Public capture API ---

    /** Sanitizes a raw URL (redacts userinfo, secret query params). */
    CapturedDiagnosticText capture_url(std::string_view raw_url);

    /** Sanitizes headers (limits count, redacts secret keys). */
    std::vector<std::pair<std::string, std::string>> capture_headers(
        const std::vector<std::pair<std::string, std::string>>& headers);

    /** Sanitizes a JSON body with known credential values. */
    CapturedDiagnosticText capture_json(
        std::string_view raw_json,
        const std::vector<std::string>& credential_values = {});

    /** Sanitizes a single SSE wire line (data: / event: / raw JSON). */
    CapturedDiagnosticText capture_wire_line(std::string_view raw_line);

    /** Sanitizes free-form content (message text, tool results). */
    CapturedDiagnosticText capture_content(std::string_view content);

    // --- Export projections (re-apply rules for export) ---

    /** JSON export: preserves content, masks credentials. */
    CapturedDiagnosticText redact_json_content(const CapturedDiagnosticText& captured);

    /** Wire line export: preserves content, masks credentials. */
    CapturedDiagnosticText redact_wire_content(const CapturedDiagnosticText& captured);

    /** Content export: masks credentials in free-form text. */
    CapturedDiagnosticText redact_content(const CapturedDiagnosticText& captured);

    /** Safe identifier for keys/IDs (stronger redaction). */
    std::string safe_identifier(std::string_view value);

private:
    // --- Internal implementation ---

    struct HttpUrl {
        std::string scheme;
        std::string username;
        std::string password;
        std::string host;
        int port = -1;
        std::string path;
        std::vector<std::pair<std::string, std::string>> query_params;
        std::string fragment;
        bool valid = false;

        std::string to_string() const;
        static std::optional<HttpUrl> parse(std::string_view raw);
        HttpUrl new_builder() const { return *this; }
    };

    static constexpr const char* REDACTED_SECRET = "[REDACTED_SECRET]";
    static constexpr const char* INVALID_URL = "[UNAVAILABLE_INVALID_URL]";
    static constexpr std::size_t MAX_IDENTIFIER_CHARS = 1024;
    static constexpr std::size_t MAX_HEADERS = 128;
    static constexpr const char* CONCH_KEY_PREFIX = "xconch";

    const std::size_t max_payload_bytes_;
    json::Value json_;  // reusable Json instance (ignoreUnknownKeys = true)

    // --- Secret detection ---
    static bool is_secret_key(std::string_view key);
    static std::string normalize_key(std::string_view key);

    // --- Redaction primitives ---
    static std::string redact_credential_values(std::string_view value);
    static std::string redact_request_metadata(std::string_view value);
    static std::string redact_known_credentials(
        std::string value, const std::vector<std::string>& credential_values);

    // --- JSON credential sanitization ---
    std::string credential_text(
        std::string_view raw_json, const std::vector<std::string>& credential_values);
    json::Value sanitize_credential_element(
        const json::Value& element, const std::vector<std::string>& credential_values);

    // --- Helpers ---
    CapturedDiagnosticText capture(
        std::string_view original, std::string_view sanitized, bool already_truncated = false);
    CapturedDiagnosticText project(
        const CapturedDiagnosticText& original, std::string_view sanitized);
    static std::pair<std::string, bool> bounded_input(std::string_view value, std::size_t max_bytes);
    static std::string decode_utf8_prefix(std::string_view bytes, std::size_t max_bytes);
    static bool is_sse_control(std::string_view trimmed);

    // --- Regex patterns (compiled on first use) ---
    static const std::vector<std::string>& secret_keys();
    static const std::vector<std::string>& secret_key_suffixes();
};

}  // namespace agora::core::diagnostic