#include "agora/core/shell_crypto.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/crypto_primitives.hpp"
#include "agora/core/sha256.hpp"

// The Kotlin reference is util/ShellCrypto.kt: every function below mirrors one public member
// of that object, including the exact failure descriptions its callers surface. All primitive
// work (SHA-256, HMAC, HKDF, base64, X25519, AES-GCM, randomness) lives in
// crypto_primitives.hpp, which carries no third-party dependency.

namespace agora::core::shell_crypto {

namespace {

constexpr const char* kHkdfInfo = "conch-agora-v2";
constexpr const char* kResponseAuthInfo = "conch-response-auth-v1";

std::string to_hex(std::span<const std::uint8_t> bytes) {
    std::ostringstream out;
    for (std::uint8_t b : bytes) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return out.str();
}

std::string b64url(std::span<const std::uint8_t> bytes) {
    return crypto::base64_encode(bytes, /*url_safe=*/true, /*padding=*/false);
}

/** java.util-style decode through the vendored URL_SAFE path; error carries the description. */
crypto::Base64Decode decode_url(std::string_view input) {
    return crypto::base64_decode(input, /*url_safe=*/true);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Key Generation (X25519)
// ---------------------------------------------------------------------------------------------

KeyPair generate_ephemeral_keypair() {
    crypto::Bytes32 seed{};
    crypto::random_bytes(seed);

    const std::array<std::uint8_t, 48> der = crypto::pkcs8_x25519_encode(seed);
    PrivateKey private_key(der.begin(), der.end());
    return {std::move(private_key), crypto::x25519_public_key(seed)};
}

std::string encode_public_key(const PublicKeyRaw& raw) {
    // X25519 SPKI encoding deterministically ends with the 32-byte raw key, so encoding the
    // raw key IS what ShellCrypto.encodePublicKey produces.
    return b64url(std::span<const std::uint8_t>(raw.data(), raw.size()));
}

DecodeResult decode_public_key(std::string_view base64url_key) {
    const crypto::Base64Decode decoded = decode_url(base64url_key);
    if (!decoded.bytes) {
        return {PublicKeyRaw{}, decoded.error.empty() ? "invalid base64url" : decoded.error};
    }
    if (decoded.bytes->size() != X25519_KEY_SIZE) {
        return {PublicKeyRaw{}, "invalid X25519 public key length: " +
                                    std::to_string(decoded.bytes->size())};
    }
    PublicKeyRaw raw{};
    std::copy(decoded.bytes->begin(), decoded.bytes->end(), raw.begin());
    return {raw, std::nullopt};
}

// ---------------------------------------------------------------------------------------------
// ECDH + HKDF
// ---------------------------------------------------------------------------------------------

DerivedKeyResult derive_aes_key(std::string_view private_key_pkcs8,
                                const PublicKeyRaw& public_key) {
    // The caller hands over DER PKCS#8 bytes (the trace driver base64url-decodes first), the
    // layout java.security.spec.PKCS8EncodedKeySpec consumes.
    const auto* der = reinterpret_cast<const std::uint8_t*>(private_key_pkcs8.data());
    const std::optional<crypto::Bytes32> seed =
        crypto::pkcs8_x25519_decode(std::span<const std::uint8_t>(der, private_key_pkcs8.size()));
    if (!seed) throw std::runtime_error("invalid PKCS#8 private key");

    const crypto::Bytes32 shared = crypto::x25519(*seed, public_key);

    // RFC 5869: raw Diffie-Hellman output needs Extract (32-byte zero salt) before Expand.
    const crypto::Bytes32 zero{};
    const crypto::Bytes32 prk = crypto::hkdf_extract(zero, shared);
    const std::vector<std::uint8_t> okm = crypto::hkdf_expand(prk, kHkdfInfo, AES_KEY_SIZE);

    DerivedKeyResult result;
    std::copy(okm.begin(), okm.end(), result.key.begin());
    return result;
}

// ---------------------------------------------------------------------------------------------
// AES-256-GCM
// ---------------------------------------------------------------------------------------------

EncryptResult encrypt(const AESKey& key, std::string_view plaintext) {
    std::array<std::uint8_t, GCM_NONCE_SIZE> nonce{};
    crypto::random_bytes(nonce);

    const std::vector<std::uint8_t> sealed =
        crypto::aes_256_gcm_encrypt(key, nonce, plaintext);  // ciphertext || tag

    std::vector<std::uint8_t> combined;
    combined.reserve(nonce.size() + sealed.size());
    combined.insert(combined.end(), nonce.begin(), nonce.end());
    combined.insert(combined.end(), sealed.begin(), sealed.end());

    EncryptResult result;
    result.ciphertext = b64url(combined);
    return result;
}

DecryptResult decrypt(const AESKey& key, std::string_view encoded) {
    const crypto::Base64Decode decoded = decode_url(encoded);
    if (!decoded.bytes) {
        return {std::string{}, decoded.error.empty() ? "invalid base64url" : decoded.error};
    }
    const std::vector<std::uint8_t>& raw = *decoded.bytes;

    if (raw.size() < GCM_NONCE_SIZE + 1) {
        return {std::string{},
                "ciphertext too short: " + std::to_string(raw.size()) + " bytes"};
    }
    const std::size_t body = raw.size() - GCM_NONCE_SIZE;
    if (body < GCM_TAG_SIZE) {
        // SunJCE refuses to hand a decrypt buffer shorter than the tag to the tag check at all;
        // the frozen trace pins the exact wrapped description.
        return {std::string{}, "javax.crypto.ShortBufferException: Output buffer invalid"};
    }

    const std::span<const std::uint8_t> nonce_span(raw.data(), GCM_NONCE_SIZE);
    std::array<std::uint8_t, GCM_NONCE_SIZE> nonce{};
    std::copy(nonce_span.begin(), nonce_span.end(), nonce.begin());
    const std::span<const std::uint8_t> sealed(raw.data() + GCM_NONCE_SIZE, body);

    const std::optional<std::vector<std::uint8_t>> plaintext =
        crypto::aes_256_gcm_decrypt(key, nonce, sealed, {});
    // SunJCE's AEADBadTagException surfaces as "Tag mismatch!" once the tag check runs.
    if (!plaintext) return {std::string{}, "Tag mismatch!"};

    return {std::string(plaintext->begin(), plaintext->end()), std::nullopt};
}

// ---------------------------------------------------------------------------------------------
// HMAC-SHA256 signing
// ---------------------------------------------------------------------------------------------

SignResult sign(std::string_view api_key, std::int64_t timestamp, std::string_view method,
                std::string_view path, std::string_view body_sha256, std::string_view nonce,
                std::string_view client_pub_key) {
    // Canonical string: "timestamp|method|path|body_sha256|nonce|client_pub_key".
    // The api key is the HMAC key and never part of the signed message.
    std::ostringstream oss;
    oss << timestamp << '|' << method << '|' << path << '|' << body_sha256 << '|' << nonce << '|'
        << client_pub_key;

    const auto* key = reinterpret_cast<const std::uint8_t*>(api_key.data());
    const auto mac =
        crypto::hmac_sha256(std::span<const std::uint8_t>(key, api_key.size()), oss.str());
    return {to_hex(mac)};
}

SHA256Result sha256_hex(std::string_view data) {
    return {agora::core::sha256(data)};
}

// ---------------------------------------------------------------------------------------------
// Response authentication
// ---------------------------------------------------------------------------------------------

std::string response_signature(std::span<const std::uint8_t> session_key, int status,
                               std::string_view body) {
    // Kotlin: the session key already is a PRK, so it is only Expanded with the
    // "conch-response-auth-v1" info (no Extract step); the HMAC covers
    // "conch-response-v1|<status>|<sha256hex(body)>".
    const std::vector<std::uint8_t> key =
        crypto::hkdf_expand(session_key, kResponseAuthInfo, AES_KEY_SIZE);
    const std::string payload =
        "conch-response-v1|" + std::to_string(status) + "|" + to_hex(agora::core::sha256(body));
    const auto mac = crypto::hmac_sha256(std::span<const std::uint8_t>(key.data(), key.size()),
                                         payload);
    return to_hex(mac);
}

bool verify_response_signature(std::span<const std::uint8_t> session_key, int status,
                               std::string_view body, std::string_view signature) {
    // Kotlin short-circuits on the key/signature shape before hashing anything and compares
    // with MessageDigest.isEqual (constant time).
    if (session_key.size() != SHA256_SIZE || signature.size() != SHA256_SIZE * 2) {
        return false;
    }
    const std::string expected = response_signature(session_key, status, body);
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<std::uint8_t>(expected[i]) ^ static_cast<std::uint8_t>(signature[i]);
    }
    return diff == 0;
}

// ---------------------------------------------------------------------------------------------
// Nonce generation
// ---------------------------------------------------------------------------------------------

std::string generate_nonce() {
    // ShellCrypto.generateNonce draws 12 random bytes (16 base64url chars).
    std::array<std::uint8_t, NONCE_SIZE> nonce{};
    crypto::random_bytes(nonce);
    return b64url(nonce);
}

// ---------------------------------------------------------------------------------------------
// base64url helpers
// ---------------------------------------------------------------------------------------------

std::string base64_url_encode(std::string_view input) {
    return b64url(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()),
                                                input.size()));
}

std::optional<std::string> base64_url_decode(std::string_view input) {
    const crypto::Base64Decode decoded = decode_url(input);
    if (!decoded.bytes) return std::nullopt;
    return std::string(decoded.bytes->begin(), decoded.bytes->end());
}

}  // namespace agora::core::shell_crypto
