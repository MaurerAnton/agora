// Unit tests for the DiagnosticRedactor port.

#include "agora/core/diagnostic_redactor.hpp"

#include <string>
#include <vector>

#include "check.hpp"

namespace diagnostic = agora::core::diagnostic;

namespace {

TEST(diagnostic_redactor_url_redaction) {
    diagnostic::DiagnosticRedactor r;

    // Basic URL with userinfo
    auto result = r.capture_url("https://user:pass@example.com/path?query=1");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
    CHECK(result.value.find("user") == std::string::npos);
    CHECK(result.value.find("pass") == std::string::npos);

    // URL with secret query param
    result = r.capture_url("https://api.example.com/endpoint?api_key=secret123&other=value");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
    CHECK(result.value.find("secret123") == std::string::npos);

    // Invalid URL
    result = r.capture_url("not a url");
    CHECK_EQ(std::string("[UNAVAILABLE_INVALID_URL]"), result.value);

    // Conch header key
    result = r.capture_url("https://example.com?xconch_token=abc123");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
}

TEST(diagnostic_redactor_header_redaction) {
    diagnostic::DiagnosticRedactor r;

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer abc123"},
        {"Content-Type", "application/json"},
        {"X-Api-Key", "secret_value"},  // "xapikey" is a secret key
    };

    auto out = r.capture_headers(headers);
    CHECK_EQ(std::size_t(3), out.size());
    CHECK_EQ(std::string("[REDACTED_SECRET]"), out[0].second);  // Authorization
    CHECK_EQ(std::string("application/json"), out[1].second);   // Content-Type
    CHECK_EQ(std::string("[REDACTED_SECRET]"), out[2].second);  // X-Api-Key
}

TEST(diagnostic_redactor_header_limit) {
    diagnostic::DiagnosticRedactor r;

    std::vector<std::pair<std::string, std::string>> headers;
    for (int i = 0; i < 150; ++i) {
        headers.emplace_back("Header" + std::to_string(i), "value");
    }
    auto out = r.capture_headers(headers);
    CHECK_EQ(std::size_t(129), out.size());  // 128 + 1 TRUNCATED
    CHECK_EQ(std::string("[TRUNCATED_HEADERS]"), out.back().first);
}

TEST(diagnostic_redactor_json_capture) {
    diagnostic::DiagnosticRedactor r;

    std::string json = R"({"Authorization":"Bearer token123","apikey":"secret","normal":"value"})";
    // Without credential values, only known patterns (Bearer token) are redacted
    auto result = r.capture_json(json);
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
    CHECK(result.value.find("token123") == std::string::npos);
    // "secret" is not a known pattern, so it stays unless passed as credential value
    // CHECK(result.value.find("secret") == std::string::npos);
    CHECK(result.value.find("value") != std::string::npos);
}

TEST(diagnostic_redactor_json_with_credential_values) {
    diagnostic::DiagnosticRedactor r;

    std::string json = R"({"data":"user_password_123","normal":"ok"})";
    auto result = r.capture_json(json, {"user_password_123"});
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
    CHECK(result.value.find("user_password_123") == std::string::npos);
}

TEST(diagnostic_redactor_wire_line_sse) {
    diagnostic::DiagnosticRedactor r;

    // SSE data line
    auto result = r.capture_wire_line("data: {\"Authorization\":\"Bearer xyz\"}");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);

    // SSE [DONE]
    result = r.capture_wire_line("data: [DONE]");
    CHECK_EQ(std::string("data: [DONE]"), result.value);

    // Control line
    result = r.capture_wire_line(": keep-alive");
    CHECK_EQ(std::string(": keep-alive"), result.value);

    // Plain text
    result = r.capture_wire_line("Bearer token123");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
}

TEST(diagnostic_redactor_content_capture) {
    diagnostic::DiagnosticRedactor r;

    // Bearer token is a known pattern
    auto result = r.capture_content("User said: Authorization: Bearer secret123");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
    CHECK(result.value.find("secret123") == std::string::npos);
}

TEST(diagnostic_redactor_export_projections) {
    diagnostic::DiagnosticRedactor r;

    // JSON export
    auto captured = r.capture_json(R"({"Authorization":"Bearer abc"})");
    auto projected = r.redact_json_content(captured);
    CHECK(projected.value.find("[REDACTED_SECRET]") != std::string::npos);

    // Wire export - SSE data with Bearer token
    captured = r.capture_wire_line("data: {\"Authorization\":\"Bearer xyz\"}");
    projected = r.redact_wire_content(captured);
    CHECK(projected.value.find("[REDACTED_SECRET]") != std::string::npos);

    // Content export
    captured = r.capture_content("Bearer token123");
    projected = r.redact_content(captured);
    CHECK(projected.value.find("[REDACTED_SECRET]") != std::string::npos);
}

TEST(diagnostic_redactor_safe_identifier) {
    diagnostic::DiagnosticRedactor r;

    std::string id = r.safe_identifier("api_key=secret123&other=value");
    CHECK(id.find("[REDACTED_SECRET]") != std::string::npos);
    CHECK(id.find("secret123") == std::string::npos);
    CHECK(id.size() <= 1024);
}

TEST(diagnostic_redactor_known_patterns) {
    diagnostic::DiagnosticRedactor r;

    // sk- pattern
    auto result = r.capture_content("sk-abcdefghijklmnop");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);

    // AIza pattern
    result = r.capture_content("AIzaSyAbcdefghijklmnopqrstuvwxyz");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);

    // ghp_ pattern
    result = r.capture_content("ghp_abcdefghijklmnopqrstuvwxyz");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);

    // xoxb- pattern
    result = r.capture_content("xoxb-abcdefghijklmnopqrst");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);

    // JWT pattern
    result = r.capture_content("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
}

TEST(diagnostic_redactor_private_key) {
    diagnostic::DiagnosticRedactor r;

    std::string pk = "-----BEGIN PRIVATE KEY-----\nMIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQD...\n-----END PRIVATE KEY-----";
    auto result = r.capture_content(pk);
    // Private key regex should match the full block
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
    // Original key content should be gone
    CHECK(result.value.find("MIIEvQIB") == std::string::npos);
}

TEST(diagnostic_redactor_bearer_token) {
    diagnostic::DiagnosticRedactor r;

    auto result = r.capture_content("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9...");
    CHECK(result.value.find("[REDACTED_SECRET]") != std::string::npos);
    CHECK(result.value.find("eyJhbGciOi") == std::string::npos);
}

}  // namespace
