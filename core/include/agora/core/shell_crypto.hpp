#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::shell_crypto {

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------

constexpr std::size_t X25519_KEY_SIZE = 32;
constexpr std::size_t X25519_SPKI_PREFIX_SIZE = 12;
constexpr std::size_t AES_KEY_SIZE = 32;
constexpr std::size_t GCM_NONCE_SIZE = 12;
constexpr std::size_t GCM_TAG_SIZE = 16;
constexpr std::size_t SHA256_SIZE = 32;
constexpr std::size_t NONCE_SIZE = 12;  // generateNonce draws 12 bytes (16 base64url chars)

// Hardcoded X25519 SPKI prefix (12 bytes): SEQUENCE(0x30, 0x2a=42) + AlgorithmIdentifier + BIT STRING header
// 30 2a 30 05 06 03 2b 65 6e 03 21 00
inline constexpr std::array<uint8_t, X25519_SPKI_PREFIX_SIZE> X25519_SPKI_PREFIX = {
    0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x6e, 0x03, 0x21, 0x00
};

// ---------------------------------------------------------------------------------------------
// Key Types
// ---------------------------------------------------------------------------------------------

using PrivateKey = std::vector<uint8_t>;  // PKCS#8 DER
using PublicKeyRaw = std::array<uint8_t, X25519_KEY_SIZE>;
using AESKey = std::array<uint8_t, AES_KEY_SIZE>;
using SHA256 = std::array<uint8_t, SHA256_SIZE>;
using Signature = std::string;  // Hex encoded

// ---------------------------------------------------------------------------------------------
// Result Types
// ---------------------------------------------------------------------------------------------

struct KeyPair {
    PrivateKey private_key;   // DER PKCS#8, exactly java.security.PrivateKey.encoded
    PublicKeyRaw public_key;  // Raw 32-byte public key
};

struct EncryptResult {
    std::string ciphertext;  // Base64url encoded: nonce(12) + ciphertext + tag(16)
};

struct DecryptResult {
    std::string decrypted;
    std::optional<std::string> error;
};

struct SignResult {
    Signature signature;  // Hex encoded
};

struct SHA256Result {
    SHA256 hash;
};

struct DerivedKeyResult {
    AESKey key;
};

struct DecodeResult {
    PublicKeyRaw public_key_raw;
    std::optional<std::string> error;
};

// ---------------------------------------------------------------------------------------------
// Core Functions
// ---------------------------------------------------------------------------------------------

// Generate X25519 ephemeral key pair
KeyPair generate_ephemeral_keypair();

// Encode raw 32-byte public key to base64url (no padding)
std::string encode_public_key(const PublicKeyRaw& raw);

// Decode base64url public key to raw 32 bytes
DecodeResult decode_public_key(std::string_view base64url_key);

// Derive AES key using ECDH + HKDF-SHA256
// private_key_pkcs8: DER PKCS#8 bytes (the caller base64url-decodes the wire form first, as the
// trace driver does), public_key: raw 32 bytes
DerivedKeyResult derive_aes_key(std::string_view private_key_pkcs8, const PublicKeyRaw& public_key);

// Encrypt plaintext with AES-256-GCM
// Returns base64url encoded: nonce(12) + ciphertext + tag(16)
EncryptResult encrypt(const AESKey& key, std::string_view plaintext);

// Decrypt base64url encoded ciphertext with AES-256-GCM
// ciphertext format: base64url(nonce(12) + ciphertext + tag(16))
DecryptResult decrypt(const AESKey& key, std::string_view ciphertext_b64url);

// Sign with HMAC-SHA256
// Returns hex-encoded signature
SignResult sign(std::string_view api_key, int64_t timestamp, std::string_view method,
                std::string_view path, std::string_view body_sha256,
                std::string_view nonce, std::string_view client_pub_key);

// Compute SHA256 hex
SHA256Result sha256_hex(std::string_view data);

// Generate response signature (HMAC-SHA256 of status + body)
std::string response_signature(std::span<const uint8_t> session_key, int status, std::string_view body);

// Verify response signature
bool verify_response_signature(std::span<const uint8_t> session_key, int status,
                               std::string_view body, std::string_view signature);

// Generate secure random nonce (12 bytes, base64url encoded -> 16 chars)
std::string generate_nonce();

// Base64 URL-safe encode (no padding)
std::string base64_url_encode(std::string_view input);

// Base64 URL-safe decode (no padding required)
std::optional<std::string> base64_url_decode(std::string_view input);

}  // namespace agora::core::shell_crypto