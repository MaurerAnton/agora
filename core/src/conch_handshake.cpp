#include "agora/core/conch_handshake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/crypto_primitives.hpp"
#include "agora/core/json.hpp"

namespace agora::core::conch {

// ---------------------------------------------------------------------------------------------
// HMAC-SHA256 Helper
// ---------------------------------------------------------------------------------------------

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
    const auto* key_bytes = reinterpret_cast<const std::uint8_t*>(key.data());
    const auto mac =
        crypto::hmac_sha256(std::span<const std::uint8_t>(key_bytes, key.size()), data);

    std::ostringstream oss;
    for (std::uint8_t b : mac) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

// ---------------------------------------------------------------------------------------------
// JSON Helper for optional string values
// ---------------------------------------------------------------------------------------------

static std::string json_get_string(const json::Value& obj, std::string_view key, std::string_view default_val = "") {
    for (const auto& member : obj.members) {
        if (member.key == key) {
            if (member.value.kind == json::Kind::String) {
                return member.value.text;
            }
        }
    }
    return std::string(default_val);
}

// ---------------------------------------------------------------------------------------------
// Handshake Implementation
// ---------------------------------------------------------------------------------------------

Handshake Handshake::decode(std::string_view json_str) {
    auto parse_result = json::parse(json_str);
    if (!parse_result.value) {
        throw std::runtime_error("JSON parse error: " + parse_result.error);
    }
    const auto& obj = parse_result.value.value();
    if (obj.kind != json::Kind::Object) {
        throw std::runtime_error("Expected JSON object");
    }

    Handshake hs;
    hs.public_key = json_get_string(obj, "public_key");
    hs.nonce = json_get_string(obj, "nonce");
    hs.signature = json_get_string(obj, "signature");
    hs.features = json_get_string(obj, "features");
    hs.challenge = json_get_string(obj, "challenge");
    hs.features_signature = json_get_string(obj, "features_signature");
    return hs;
}

std::string Handshake::security_payload() const {
    return "conch-handshake-v2|" + public_key + "|" + challenge + "|" + features;
}

bool Handshake::verify(std::string_view api_key, std::string_view expected_challenge) const {
    // Check expected challenge is not empty and matches
    if (expected_challenge.empty() || challenge != expected_challenge) {
        return false;
    }

    // Check nonce is not empty
    if (nonce.empty()) {
        return false;
    }

    // Verify main signature: HMAC-SHA256(api_key, "nonce|public_key")
    if (!verify_payload(api_key, nonce, public_key, signature)) {
        return false;
    }

    // Verify features_signature: HMAC-SHA256(api_key, "nonce|security_payload")
    if (!verify_payload(api_key, nonce, security_payload(), features_signature)) {
        return false;
    }

    // Check required features
    std::vector<std::string_view> features_list;
    std::string_view features_sv = features;
    size_t pos = 0;
    while (pos < features_sv.size()) {
        size_t next = features_sv.find(',', pos);
        if (next == std::string_view::npos) {
            features_list.push_back(features_sv.substr(pos));
            break;
        }
        features_list.push_back(features_sv.substr(pos, next - pos));
        pos = next + 1;
    }

    for (const auto& required : REQUIRED_FEATURES) {
        bool found = false;
        for (const auto& f : features_list) {
            if (f == required) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

bool Handshake::verify_payload(std::string_view api_key, std::string_view nonce,
                               std::string_view payload, std::string_view signature) {
    if (signature.size() != 64) {  // SHA256 hex = 64 chars
        return false;
    }

    std::string payload_with_nonce = std::string(nonce) + "|" + std::string(payload);
    std::string expected = hmac_sha256_hex(api_key, payload_with_nonce);

    // Constant-time comparison
    if (expected.size() != signature.size()) {
        return false;
    }
    int result = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        result |= static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(signature[i]);
    }
    return result == 0;
}

}  // namespace agora::core::conch