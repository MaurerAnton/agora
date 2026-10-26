#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::conch {

// ---------------------------------------------------------------------------------------------
// ConchHandshake.kt - Handshake verification for Conch protocol
// ---------------------------------------------------------------------------------------------

// Required features for Conch handshake
inline constexpr std::array<std::string_view, 4> REQUIRED_FEATURES = {
    "hkdf-extract-v2", "response-auth-v1", "sse-sequence-v1", "rejection-auth-v1"
};

// ---------------------------------------------------------------------------------------------
// Data Structures
// ---------------------------------------------------------------------------------------------

struct Handshake {
    std::string public_key;
    std::string nonce;
    std::string signature;
    std::string features = "";
    std::string challenge = "";
    std::string features_signature = "";

    // Decode from JSON string
    static Handshake decode(std::string_view json_str);

    // Verify the handshake
    // Returns true if all verification checks pass
    bool verify(std::string_view api_key, std::string_view expected_challenge) const;

private:
    // Verify a payload using HMAC-SHA256 with api_key
    static bool verify_payload(std::string_view api_key, std::string_view nonce,
                               std::string_view payload, std::string_view signature);

    // Build the security payload string
    std::string security_payload() const;
};

// ---------------------------------------------------------------------------------------------
// HMAC-SHA256 Helper
// ---------------------------------------------------------------------------------------------

// Compute HMAC-SHA256 and return hex-encoded string
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

}  // namespace agora::core::conch