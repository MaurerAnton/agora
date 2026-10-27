#include "agora/core/shell_client.hpp"

#include <algorithm>
#include <chrono>
#include <map>

#include "agora/core/json.hpp"
#include "agora/core/sha256.hpp"

namespace agora::core::shell_client {
namespace {

namespace json = agora::core::json;

// ---------------------------------------------------------------------------------------------
// JSON access helpers (kotlinx JsonObject/JsonPrimitive semantics)
// ---------------------------------------------------------------------------------------------

const json::Value* member_last(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

bool is_primitive(const json::Value& value) {
    return value.kind == json::Kind::String || value.kind == json::Kind::Literal;
}

std::string kotlin_class_name(const json::Value& value) {
    switch (value.kind) {
        case json::Kind::Object:
            return "class kotlinx.serialization.json.JsonObject";
        case json::Kind::Array:
            return "class kotlinx.serialization.json.JsonArray";
        default:
            return "class kotlinx.serialization.json.JsonLiteral";
    }
}

/** `JsonElement.jsonPrimitive.content` — throws like the kotlinx accessor on other kinds. */
std::string primitive_content(const json::Value& value) {
    if (!is_primitive(value)) {
        throw ClientError("Element " + kotlin_class_name(value) + " is not a JsonPrimitive");
    }
    return value.text;
}

/** `JsonElement.jsonArray` — throws like the kotlinx accessor on other kinds. */
const json::Value& require_array(const json::Value* value) {
    if (value == nullptr) {
        static json::Value empty_array = [] {
            json::Value v;
            v.kind = json::Kind::Array;
            return v;
        }();
        return empty_array;
    }
    if (value->kind != json::Kind::Array) {
        throw ClientError("Element " + kotlin_class_name(*value) + " is not a JsonArray");
    }
    return *value;
}

std::optional<std::int64_t> parse_signed(std::string_view text, std::uint64_t limit) {
    if (text.empty()) return std::nullopt;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        index = 1;
    } else if (text[0] == '+') {
        index = 1;
    }
    if (index >= text.size()) return std::nullopt;
    std::uint64_t accumulator = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (accumulator > (limit - digit) / 10) return std::nullopt;
        accumulator = accumulator * 10 + digit;
    }
    if (negative) return -static_cast<std::int64_t>(accumulator);
    return static_cast<std::int64_t>(accumulator);
}

std::optional<std::int32_t> to_int_or_null(std::string_view text) {
    const auto value = parse_signed(text, 2147483648ULL);
    if (!value || value < -2147483648LL || value > 2147483647LL) return std::nullopt;
    return static_cast<std::int32_t>(*value);
}

std::optional<std::int64_t> to_long_or_null(std::string_view text) {
    return parse_signed(text, 9223372036854775808ULL);
}

std::optional<bool> to_boolean_strict(std::string_view text) {
    if (text == "true") return true;
    if (text == "false") return false;
    return std::nullopt;
}

/** `json["key"]?.jsonPrimitive?.content` — missing key or JSON null handled by the caller. */
std::optional<std::string> member_content(const json::Value& object, std::string_view key) {
    const json::Value* member = member_last(object, key);
    if (member == nullptr) return std::nullopt;
    return primitive_content(*member);
}

// ---------------------------------------------------------------------------------------------
// JSON building (kotlinx buildJsonObject + toString())
// ---------------------------------------------------------------------------------------------

json::Value jstr(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value jnum(std::string digits) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(digits);
    return value;
}

json::Value jobj(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

std::string json_body_command(const std::string& command, std::int32_t timeout_ms,
                              const std::string& workdir) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("command", jstr(command));
    members.emplace_back("timeout_ms", jnum(std::to_string(timeout_ms)));
    if (!json::is_blank(workdir)) members.emplace_back("workdir", jstr(workdir));
    return json::to_kotlin_string(jobj(std::move(members)));
}

// ---------------------------------------------------------------------------------------------
// ShellCrypto mirror (on core's primitives)
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kHkdfInfo = "conch-agora-v2";

std::string hex_lower(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::string sha256_hex(std::string_view data) {
    return hex_lower(sha256(data));
}


bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}





bool verify_response_signature(const crypto::Bytes32& session_key, std::int32_t status,
                               std::string_view body, std::string_view signature) {
    return signature.size() == 64 &&
           constant_time_equal(response_signature(session_key, status, body), signature);
}

std::string generate_nonce() {
    std::array<std::uint8_t, 12> bytes{};
    crypto::random_bytes(bytes);
    return crypto::base64_encode(bytes, /*url_safe=*/true, /*padding=*/false);
}

std::int64_t epoch_seconds_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------------------------
// ConchHandshake mirror
// ---------------------------------------------------------------------------------------------

struct Handshake {
    std::string public_key;
    std::string nonce;
    std::string signature;
    std::string features;
    std::string challenge;
    std::string features_signature;
};

Handshake decode_handshake(const std::string& body) {
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        // kotlinx JsonDecodingException path (unpinned edge; any message works here).
        throw ClientError("Handshake body is not a JSON object");
    }
    Handshake handshake;
    const auto required = [&](std::string_view key) {
        const auto content = member_content(*parsed.value, key);
        if (!content) {
            // kotlinx MissingFieldException for the required @Serializable properties.
            throw ClientError("Field '" + std::string(key) + "' is missing");
        }
        return *content;
    };
    handshake.public_key = required("public_key");
    handshake.nonce = required("nonce");
    handshake.signature = required("signature");
    handshake.features = member_content(*parsed.value, "features").value_or("");
    handshake.challenge = member_content(*parsed.value, "challenge").value_or("");
    handshake.features_signature =
        member_content(*parsed.value, "features_signature").value_or("");
    return handshake;
}

bool verify_payload(std::string_view api_key, std::string_view nonce,
                    std::string_view payload, std::string_view signature) {
    if (signature.size() != 64) return false;
    return constant_time_equal(hmac_hex(api_key, std::string(nonce) + "|" + std::string(payload)),
                               signature);
}

std::vector<std::string> split_commas(const std::string& text) {
    // Kotlin String.split(',') keeps empty segments.
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = text.find(',', start);
        if (comma == std::string::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, comma - start));
        start = comma + 1;
    }
}

bool verify_handshake(const Handshake& handshake, std::string_view api_key,
                      std::string_view expected_challenge) {
    if (expected_challenge.empty()) return false;
    if (handshake.challenge != expected_challenge) return false;
    if (handshake.nonce.empty()) return false;
    if (!verify_payload(api_key, handshake.nonce, handshake.public_key, handshake.signature)) {
        return false;
    }
    const std::string security_payload = "conch-handshake-v2|" + handshake.public_key + "|" +
                                         handshake.challenge + "|" + handshake.features;
    if (!verify_payload(api_key, handshake.nonce, security_payload,
                        handshake.features_signature)) {
        return false;
    }
    static constexpr std::string_view kRequired[] = {
        "hkdf-extract-v2", "response-auth-v1", "sse-sequence-v1", "rejection-auth-v1",
    };
    const std::vector<std::string> features = split_commas(handshake.features);
    for (const std::string_view required : kRequired) {
        if (std::find(features.begin(), features.end(), required) == features.end()) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Small shared builders
// ---------------------------------------------------------------------------------------------

std::string take_prefix(const std::string& text, std::size_t count) {
    return text.substr(0, std::min(count, text.size()));
}

/** Kotlin's two http-error sites: `Conch at $url returned HTTP $code: $detail`. */
std::string http_error_text(const std::string& server_url, std::int32_t code,
                            const std::string& body) {
    const std::string detail = take_prefix(body, 240);
    return "Conch at " + server_url + " returned HTTP " + std::to_string(code) + ": " +
           (json::is_blank(detail) ? "empty response" : detail);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ShellCrypto compositions (declared in the header; defined at namespace scope)
// ---------------------------------------------------------------------------------------------

std::string hmac_hex(std::string_view key, std::string_view data) {
    return hex_lower(crypto::hmac_sha256(std::span(reinterpret_cast<const std::uint8_t*>(key.data()),
                                                   key.size()),
                                         data));
}

crypto::Bytes32 derive_aes_key(const crypto::Bytes32& private_seed,
                               const std::span<const std::uint8_t, 32> peer_public) {
    const crypto::Bytes32 shared = crypto::x25519(private_seed, peer_public);
    const crypto::Bytes32 zeros{};
    const crypto::Bytes32 prk = crypto::hkdf_extract(zeros, shared);
    const std::vector<std::uint8_t> expanded =
        crypto::hkdf_expand(prk, kHkdfInfo, 32);
    crypto::Bytes32 key{};
    std::copy(expanded.begin(), expanded.end(), key.begin());
    return key;
}

/** Kotlin ShellCrypto.encrypt: base64url(nonce12 || ciphertext || tag16), no padding. */
std::string encrypt_payload(const crypto::Bytes32& key, std::string_view plaintext) {
    crypto::Bytes12 nonce{};
    crypto::random_bytes(nonce);
    const std::vector<std::uint8_t> sealed = crypto::aes_256_gcm_encrypt(key, nonce, plaintext);
    std::vector<std::uint8_t> combined;
    combined.reserve(12 + sealed.size());
    combined.insert(combined.end(), nonce.begin(), nonce.end());
    combined.insert(combined.end(), sealed.begin(), sealed.end());
    return crypto::base64_encode(combined, /*url_safe=*/true, /*padding=*/false);
}

/** Kotlin ShellCrypto.decrypt (throws ClientError with the Kotlin messages). */
std::vector<std::uint8_t> decrypt_payload(const crypto::Bytes32& key,
                                          const std::string& encoded) {
    const crypto::Base64Decode decoded = crypto::base64_decode(encoded, /*url_safe=*/true);
    if (!decoded.bytes) throw ClientError(decoded.error);
    const std::vector<std::uint8_t>& raw = *decoded.bytes;
    if (raw.size() < 12 + 1) {
        throw ClientError("ciphertext too short: " + std::to_string(raw.size()) + " bytes");
    }
    std::array<std::uint8_t, 12> nonce{};
    std::copy(raw.begin(), raw.begin() + 12, nonce.begin());
    const std::span<const std::uint8_t> ciphertext(raw.data() + 12, raw.size() - 12);
    const auto plaintext = crypto::aes_256_gcm_decrypt(key, nonce, ciphertext);
    if (!plaintext) throw ClientError("Tag mismatch!");  // JDK AEADBadTagException message
    return *plaintext;
}

/** Kotlin ShellCrypto.responseSignature. */
std::string response_signature(const crypto::Bytes32& session_key, std::int32_t status,
                               std::string_view body) {
    const std::string info = "conch-response-auth-v1";
    const std::vector<std::uint8_t> key =
        crypto::hkdf_expand(session_key, info, 32);
    const std::string payload = "conch-response-v1|" + std::to_string(status) + "|" +
                                sha256_hex(body);
    return hex_lower(crypto::hmac_sha256(key, payload));
}

// ---------------------------------------------------------------------------------------------
// describe functions
// ---------------------------------------------------------------------------------------------

std::string describe_conch_connection_failure(const std::string& server_url,
                                              const TransportFailure& failure) {
    switch (failure.kind) {
        case TransportFailure::Kind::UnknownHost:
            return "Cannot resolve Conch host for " + server_url + ": " + failure.detail;
        case TransportFailure::Kind::Connect:
            return "Cannot connect to Conch at " + server_url + ": " + failure.detail;
        case TransportFailure::Kind::SocketTimeout:
        case TransportFailure::Kind::InterruptedIO:
            return "Conch public-key request timed out at " + server_url;
        case TransportFailure::Kind::Ssl:
            return "TLS connection to Conch at " + server_url + " failed: " + failure.detail;
        case TransportFailure::Kind::Plain:
            return "Conch public-key request to " + server_url + " failed: " + failure.detail;
    }
    return "";
}

std::string describe_conch_request_failure(const std::string& server_url,
                                           const std::string& operation,
                                           const TransportFailure& failure) {
    switch (failure.kind) {
        case TransportFailure::Kind::UnknownHost:
            return "Cannot resolve Conch host for " + server_url + ": " + failure.detail;
        case TransportFailure::Kind::Connect:
            return "Cannot connect to Conch at " + server_url + ": " + failure.detail;
        case TransportFailure::Kind::SocketTimeout:
        case TransportFailure::Kind::InterruptedIO:
            return "Conch " + operation + " timed out at " + server_url;
        case TransportFailure::Kind::Ssl:
            return "TLS connection to Conch at " + server_url + " failed: " + failure.detail;
        case TransportFailure::Kind::Plain:
            return "Conch " + operation + " to " + server_url + " failed: " + failure.detail;
    }
    return "";
}

// ---------------------------------------------------------------------------------------------
// ShellClient
// ---------------------------------------------------------------------------------------------

ShellClient::ShellClient(std::string server_url, std::string api_key,
                         std::string cached_public_key, ConchTransport& transport)
    : server_url_(std::move(server_url)),
      api_key_(std::move(api_key)),
      transport_(transport) {
    if (!json::is_blank(cached_public_key)) {
        // Kotlin: decode failure is swallowed — a fresh fetch supplies the key later.
        try {
            const crypto::Base64Decode decoded =
                crypto::base64_decode(cached_public_key, /*url_safe=*/true);
            if (decoded.bytes && decoded.bytes->size() == 32) {
                crypto::Bytes32 key{};
                std::copy(decoded.bytes->begin(), decoded.bytes->end(), key.begin());
                server_public_key_ = key;
            }
        } catch (const std::exception&) {
            // Silently ignored, like the Kotlin init block.
        }
    }
}

bool ShellClient::fetch_public_key() {
    if (security_verified_ && server_public_key_) return true;
    if (api_key_.empty()) {
        last_error_ = "Conch authentication is disabled locally; no public-key exchange is needed";
        return false;
    }
    try {
        const std::string challenge = generate_nonce();
        const ConchTextResponse response =
            transport_.get_text_response(server_url_ + "/public-key?challenge=" + challenge, {});
        if (!response.is_successful) {
            last_error_ = http_error_text(server_url_, response.code, response.body);
            return false;
        }
        const Handshake handshake = decode_handshake(response.body);
        if (!verify_handshake(handshake, api_key_, challenge)) {
            last_error_ =
                "Conch security handshake failed at " + server_url_ +
                ". Check the API key and update Conch before sending requests.";
            return false;
        }
        const crypto::Base64Decode decoded =
            crypto::base64_decode(handshake.public_key, /*url_safe=*/true);
        if (!decoded.bytes) throw ClientError(decoded.error);
        if (decoded.bytes->size() != 32) {
            throw ClientError("invalid X25519 public key length: " +
                              std::to_string(decoded.bytes->size()));
        }
        crypto::Bytes32 key{};
        std::copy(decoded.bytes->begin(), decoded.bytes->end(), key.begin());
        server_public_key_ = key;
        security_verified_ = true;
        last_error_.reset();
        return true;
    } catch (const TransportFailure& failure) {
        last_error_ = describe_conch_connection_failure(server_url_, failure);
        return false;
    } catch (const std::exception& failure) {
        last_error_ = describe_conch_connection_failure(
            server_url_,
            TransportFailure(TransportFailure::Kind::Plain, failure.what()));
        return false;
    }
}

std::optional<std::string> ShellClient::get_server_public_key_base64() const {
    if (!server_public_key_) return std::nullopt;
    return crypto::base64_encode(*server_public_key_, /*url_safe=*/true, /*padding=*/false);
}

ShellClient::PreparedRequest ShellClient::prepare_request(const std::string& command,
                                                          std::int32_t timeout_ms,
                                                          const std::string& workdir) {
    const std::string json_body = build_json_body(command, timeout_ms, workdir);

    if (api_key_.empty()) {
        PreparedRequest request;
        request.body = json_body;
        request.headers = {{"Content-Type", "application/json"}};
        request.is_encrypted = false;
        request.server_url = server_url_;
        return request;
    }
    if (!security_verified_) {
        throw ClientError("Conch security handshake is required before sending requests");
    }
    if (!server_public_key_) {
        throw ClientError(
            "Server public key not available. Call fetchPublicKey() first.");
    }

    crypto::Bytes32 ephemeral_seed{};
    crypto::random_bytes(ephemeral_seed);
    const crypto::Bytes32 aes_key = derive_aes_key(ephemeral_seed, *server_public_key_);
    current_aes_key_ = aes_key;
    current_ephemeral_seed_ = ephemeral_seed;

    const std::string encrypted_body = encrypt_payload(aes_key, json_body);
    const std::string body_sha256 = sha256_hex(encrypted_body);
    const std::int64_t timestamp = epoch_seconds_now();
    const std::string nonce = generate_nonce();
    const crypto::Bytes32 client_public = crypto::x25519_public_key(ephemeral_seed);
    const std::string client_public_key =
        crypto::base64_encode(client_public, /*url_safe=*/true, /*padding=*/false);
    const std::string signature =
        hmac_hex(api_key_,
                 std::to_string(timestamp) + "|POST|/execute|" + body_sha256 + "|" + nonce +
                     "|" + client_public_key);

    PreparedRequest request;
    request.body = encrypted_body;
    request.headers = {
        {"Content-Type", "application/octet-stream"},
        {"X-Timestamp", std::to_string(timestamp)},
        {"X-Signature", signature},
        {"X-Nonce", nonce},
        {"X-Encryption", "v2"},
        {"X-Client-Public-Key", client_public_key},
    };
    request.is_encrypted = true;
    request.server_url = server_url_;
    return request;
}

std::string ShellClient::decrypt_sse_data(const std::string& encrypted_data) {
    if (!current_aes_key_) throw ClientError("No session key");
    const std::vector<std::uint8_t> plaintext = decrypt_payload(*current_aes_key_, encrypted_data);
    return std::string(plaintext.begin(), plaintext.end());
}

std::string ShellClient::build_json_body(const std::string& command, std::int32_t timeout_ms,
                                         const std::string& workdir) {
    return json_body_command(command, timeout_ms, workdir);
}

std::string ShellClient::file_post(const std::string& path, const std::string& payload) {
    return encrypted_post(path, payload, std::nullopt);
}

std::string ShellClient::encrypted_post(const std::string& path, const std::string& payload,
                                        const std::optional<std::int64_t>& call_timeout_millis) {
    if (api_key_.empty()) {
        const ConchTextResponse response = [&] {
            try {
                return transport_.post_text_response(
                    server_url_ + path, payload,
                    {{"Content-Type", "application/json"}}, call_timeout_millis);
            } catch (const TransportFailure& failure) {
                throw ClientError(
                    describe_conch_request_failure(server_url_, path + " request", failure));
            }
        }();
        if (!response.is_successful) {
            throw ClientError(http_error_text(server_url_, response.code, response.body));
        }
        return response.body;
    }

    if (!security_verified_ && !fetch_public_key()) {
        throw ClientError(last_error_.value_or("Failed to fetch server public key"));
    }
    if (!server_public_key_) {
        throw ClientError("Server public key not available. Call fetchPublicKey() first.");
    }

    crypto::Bytes32 ephemeral_seed{};
    crypto::random_bytes(ephemeral_seed);
    const crypto::Bytes32 aes_key = derive_aes_key(ephemeral_seed, *server_public_key_);
    const std::string encrypted_body = encrypt_payload(aes_key, payload);
    const std::string body_sha256 = sha256_hex(encrypted_body);
    const std::int64_t timestamp = epoch_seconds_now();
    const std::string nonce = generate_nonce();
    const crypto::Bytes32 client_public = crypto::x25519_public_key(ephemeral_seed);
    const std::string client_public_key =
        crypto::base64_encode(client_public, /*url_safe=*/true, /*padding=*/false);
    const std::string signature =
        hmac_hex(api_key_, std::to_string(timestamp) + "|POST|" + path + "|" + body_sha256 +
                               "|" + nonce + "|" + client_public_key);
    const HeaderList headers = {
        {"Content-Type", "application/octet-stream"},
        {"X-Timestamp", std::to_string(timestamp)},
        {"X-Signature", signature},
        {"X-Nonce", nonce},
        {"X-Encryption", "v2"},
        {"X-Client-Public-Key", client_public_key},
    };

    const ConchTextResponse response = [&] {
        try {
            return transport_.post_text_response(server_url_ + path, encrypted_body, headers,
                                                 call_timeout_millis);
        } catch (const TransportFailure& failure) {
            throw ClientError(
                describe_conch_request_failure(server_url_, path + " request", failure));
        }
    }();
    if (!response.is_successful) {
        throw ClientError(http_error_text(server_url_, response.code, response.body));
    }
    if (!verify_response_signature(aes_key, response.code, response.body, response.signature)) {
        throw ClientError("Conch response authentication failed at " + server_url_);
    }
    try {
        const std::vector<std::uint8_t> plaintext = decrypt_payload(aes_key, response.body);
        return std::string(plaintext.begin(), plaintext.end());
    } catch (const ClientError& error) {
        throw ClientError("Conch response decryption failed at " + server_url_ + ": " +
                          error.what());
    }
}

// ---------------------------------------------------------------------------------------------
// file endpoints
// ---------------------------------------------------------------------------------------------

shell_tools::ShellFileReadResult ShellClient::file_read(const std::string& path,
                                                        std::int64_t offset,
                                                        std::int64_t limit) {
    constexpr std::int64_t kReadMax = 1'048'576;
    const std::int64_t effective_limit = (limit >= 1 && limit <= kReadMax) ? limit : kReadMax;
    const std::int64_t normalized_offset = std::max<std::int64_t>(offset, 0);
    const std::string payload =
        json::to_kotlin_string(jobj({{"path", jstr(path)},
                                     {"offset", jnum(std::to_string(normalized_offset))},
                                     {"limit", jnum(std::to_string(effective_limit))}}));
    const std::string response_body = file_post("/file/read", payload);
    const json::ParseResult parsed = json::parse(response_body);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw ClientError("file/read response is not a JSON object");
    }
    const json::Value& object = *parsed.value;

    if (const auto error = member_content(object, "error")) {
        shell_tools::ShellFileReadResult result;
        result.content = "";
        result.lines = 0;
        result.total_lines = 0;
        result.total_bytes = 0;
        result.returned_bytes = 0;
        result.offset = normalized_offset;
        result.limit = effective_limit;
        result.truncated = false;
        result.error = *error;
        return result;
    }
    const std::string content = member_content(object, "content").value_or("");
    const std::int64_t total_bytes =
        to_long_or_null(member_content(object, "size").value_or("")).value_or(0);
    shell_tools::ShellFileReadResult result;
    result.content = content;
    result.lines =
        to_int_or_null(member_content(object, "lines").value_or(""))
            .value_or(shell_tools::shell_file_line_count(content));
    result.total_lines = to_int_or_null(member_content(object, "totalLines").value_or("")).value_or(0);
    result.total_bytes = total_bytes;
    result.returned_bytes = static_cast<std::int64_t>(content.size());
    result.offset = std::min(normalized_offset, total_bytes);
    result.limit = effective_limit;
    const auto explicit_truncated =
        to_boolean_strict(member_content(object, "truncated").value_or(""));
    result.truncated = explicit_truncated.value_or(
        normalized_offset + static_cast<std::int64_t>(content.size()) < total_bytes);
    return result;
}

std::optional<std::string> ShellClient::file_write(const std::string& path,
                                                   const std::string& content) {
    const std::string payload = json::to_kotlin_string(
        jobj({{"path", jstr(path)}, {"content", jstr(content)}}));
    const std::string response_body = file_post("/file/write", payload);
    const json::ParseResult parsed = json::parse(response_body);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw ClientError("file/write response is not a JSON object");
    }
    return member_content(*parsed.value, "error");
}

shell_file::ShellFileEditResult ShellClient::file_edit(const std::string& path,
                                                       const std::string& old_string,
                                                       const std::string& new_string,
                                                       bool replace_all) {
    const std::string payload =
        json::to_kotlin_string(jobj({{"path", jstr(path)},
                                     {"old_string", jstr(old_string)},
                                     {"new_string", jstr(new_string)},
                                     {"replace_all", jnum(replace_all ? "true" : "false")}}));
    const std::string response_body = file_post("/file/edit", payload);
    const json::ParseResult parsed = json::parse(response_body);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw ClientError("file/edit response is not a JSON object");
    }
    const json::Value& object = *parsed.value;
    if (const auto error = member_content(object, "error")) {
        return shell_file::ShellFileEditResult{0, "", *error};
    }
    return shell_file::ShellFileEditResult{
        static_cast<int>(
            to_int_or_null(member_content(object, "replacements").value_or("")).value_or(0)),
        member_content(object, "sha256").value_or(""), std::nullopt};
}

ShellClient::GlobOutcome ShellClient::file_glob(const std::string& pattern,
                                                const std::string& path,
                                                const std::optional<std::int32_t>& depth) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("pattern", jstr(pattern));
    if (!json::is_blank(path)) members.emplace_back("path", jstr(path));
    if (depth) members.emplace_back("depth", jnum(std::to_string(*depth)));
    const std::string response_body =
        file_post("/file/glob", json::to_kotlin_string(jobj(std::move(members))));
    const json::ParseResult parsed = json::parse(response_body);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw ClientError("file/glob response is not a JSON object");
    }
    const json::Value& object = *parsed.value;
    if (const auto error = member_content(object, "error")) {
        return GlobOutcome{false, {}, false, *error};
    }
    const json::Value& files = require_array(member_last(object, "files"));
    GlobOutcome outcome;
    outcome.ok = true;
    for (const json::Value& element : files.elements) {
        outcome.files.push_back(primitive_content(element));
    }
    outcome.truncated =
        to_boolean_strict(member_content(object, "truncated").value_or("")).value_or(false);
    return outcome;
}

ShellClient::GrepOutcome ShellClient::file_grep(const std::string& pattern,
                                                const std::string& path,
                                                const std::string& glob) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("pattern", jstr(pattern));
    if (!json::is_blank(path)) members.emplace_back("path", jstr(path));
    if (!json::is_blank(glob)) members.emplace_back("glob", jstr(glob));
    const std::string response_body =
        file_post("/file/grep", json::to_kotlin_string(jobj(std::move(members))));
    const json::ParseResult parsed = json::parse(response_body);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw ClientError("file/grep response is not a JSON object");
    }
    const json::Value& object = *parsed.value;
    if (const auto error = member_content(object, "error")) {
        return GrepOutcome{false, {}, false, *error};
    }
    const json::Value& matches =
        require_array(member_last(object, "matches"));
    GrepOutcome outcome;
    outcome.ok = true;
    for (const json::Value& element : matches.elements) {
        if (element.kind != json::Kind::Object) {
            throw ClientError("Element " + kotlin_class_name(element) +
                              " is not a JsonObject");
        }
        GrepMatch match;
        match.path = member_content(element, "path").value_or("");
        match.line =
            to_int_or_null(member_content(element, "line").value_or("")).value_or(0);
        match.content = member_content(element, "content").value_or("");
        outcome.matches.push_back(std::move(match));
    }
    outcome.truncated =
        to_boolean_strict(member_content(object, "truncated").value_or("")).value_or(false);
    return outcome;
}

ShellClient::FileImageResult ShellClient::file_image(const std::string& path) {
    const std::string payload =
        json::to_kotlin_string(jobj({{"path", jstr(path)}}));
    const std::string response_body = file_post("/file/image", payload);
    const json::ParseResult parsed = json::parse(response_body);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw ClientError("file/image response is not a JSON object");
    }
    const json::Value& object = *parsed.value;
    if (const auto error = member_content(object, "error")) {
        return FileImageResult{"", "", 0, *error};
    }
    return FileImageResult{
        member_content(object, "data").value_or(""),
        member_content(object, "mimeType").value_or(""),
        to_long_or_null(member_content(object, "size").value_or("")).value_or(0),
        std::nullopt,
    };
}

// ---------------------------------------------------------------------------------------------
// jobs endpoints
// ---------------------------------------------------------------------------------------------

std::string ShellClient::start_job(const std::string& command, std::int32_t timeout_ms,
                                   const std::string& workdir) {
    return encrypted_post("/jobs/start", build_json_body(command, timeout_ms, workdir),
                          std::nullopt);
}

std::string ShellClient::list_jobs() {
    return encrypted_post("/jobs/list", "{}", std::nullopt);
}

std::string ShellClient::get_job(const std::string& job_id) {
    const std::string payload =
        json::to_kotlin_string(jobj({{"job_id", jstr(job_id)}}));
    return encrypted_post("/jobs/get", payload, std::nullopt);
}

std::string ShellClient::stop_job(const std::string& job_id,
                                  const std::optional<std::int64_t>& call_timeout_millis) {
    const std::string payload =
        json::to_kotlin_string(jobj({{"job_id", jstr(job_id)}}));
    return encrypted_post("/jobs/stop", payload, call_timeout_millis);
}

std::string ShellClient::acknowledge_job(const std::string& job_id) {
    const std::string payload =
        json::to_kotlin_string(jobj({{"job_id", jstr(job_id)}}));
    return encrypted_post("/jobs/ack", payload, std::nullopt);
}

}  // namespace agora::core::shell_client
