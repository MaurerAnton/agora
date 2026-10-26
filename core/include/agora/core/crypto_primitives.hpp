#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::crypto {

// ---------------------------------------------------------------------------------------------
// Bare-toolchain cryptographic primitives for the shell execution layer.
//
// core/ carries no third-party dependency (core/README.md "Rules for this directory"), so the
// primitives ShellCrypto.kt needs are implemented here from their specifications:
//
//   * SHA-256      FIPS 180-4 -- reuses the in-repo Sha256Digest (sha256.hpp)
//   * HMAC-SHA256  RFC 2104
//   * HKDF-SHA256  RFC 5869 (Extract and Expand)
//   * base64       RFC 4648 with java.util.Base64's failure descriptions
//   * X25519       RFC 7748 (Montgomery ladder over GF(2^255-19))
//   * PKCS#8       RFC 5958 one-private-keyInfo for id-X25519 (fixed 48-byte layout)
//   * AES-256-GCM  FIPS 197 (cipher) + NIST SP 800-38D (GCM)
//
// Every routine is pinned two ways: known-answer vectors generated from an independent
// implementation (tests/crypto_primitives_test.cpp) and the frozen shell-crypto traces, which
// cross-check the Kotlin reference against these implementations byte for byte.
// ---------------------------------------------------------------------------------------------

using Bytes32 = std::array<std::uint8_t, 32>;
using Bytes12 = std::array<std::uint8_t, 12>;

// -- digesting -----------------------------------------------------------------------------------

Bytes32 hmac_sha256(std::span<const std::uint8_t> key, std::string_view data);

/** RFC 5869 Extract: PRK = HMAC(salt, IKM). */
Bytes32 hkdf_extract(std::span<const std::uint8_t> salt, std::span<const std::uint8_t> ikm);

/** RFC 5869 Expand from an already extracted PRK (ShellCrypto.responseSignature path). */
std::vector<std::uint8_t> hkdf_expand(std::span<const std::uint8_t> prk, std::string_view info,
                                      std::size_t length);

/** RFC 5869 Extract-then-Expand; an empty salt is the all-zero HashLen salt of RFC 5869. */
std::vector<std::uint8_t> hkdf_sha256(std::string_view salt, std::string_view ikm,
                                      std::string_view info, std::size_t length);

// -- base64 --------------------------------------------------------------------------------------

/** RFC 4648 encoder; url_safe maps +/ to -_, padding appends '=' to a multiple of four. */
std::string base64_encode(std::span<const std::uint8_t> input, bool url_safe, bool padding);

struct Base64Decode {
    std::optional<std::vector<std::uint8_t>> bytes;  // set when the input decodes
    std::string error;  // java.util.Base64's description when it does not (see base64_decode)
};

/**
 * Decoder with java.util.Base64.Decoder semantics (the vendored platform/Base64.java first
 * maps -_ to +/ for URL_SAFE input, appends the missing padding, then decodes): characters
 * outside the alphabet fail with "Illegal base64 character <hex>" and malformed padding fails
 * with "Illegal base64 character 3d".
 */
Base64Decode base64_decode(std::string_view input, bool url_safe);

// -- randomness ----------------------------------------------------------------------------------

/** Fills out from the platform CSPRNG (std::random_device, the convention already used in core). */
void random_bytes(std::span<std::uint8_t> out);

// -- X25519 (RFC 7748) ---------------------------------------------------------------------------

/** Derives the public key for a 32-byte private seed (the seed is clamped as RFC 7748 requires). */
Bytes32 x25519_public_key(std::span<const std::uint8_t, 32> seed);

/** X25519 Diffie-Hellman: clamps the seed, multiplies with the peer's u-coordinate. */
Bytes32 x25519(std::span<const std::uint8_t, 32> seed, std::span<const std::uint8_t, 32> peer_public);

// -- X25519 PKCS#8 (RFC 5958 one-private-keyInfo, id-X25519) --------------------------------------

/** Encodes a 32-byte seed as the fixed 48-byte DER PrivateKeyInfo java's PKCS8EncodedKeySpec reads. */
std::array<std::uint8_t, 48> pkcs8_x25519_encode(std::span<const std::uint8_t, 32> seed);

/** Decodes a DER PrivateKeyInfo; rejects anything that is not the id-X25519 layout. */
std::optional<Bytes32> pkcs8_x25519_decode(std::span<const std::uint8_t> der);

// -- AES-256-GCM (SP 800-38D) --------------------------------------------------------------------

/** Returns ciphertext || 16-byte tag, the layout ShellCrypto.encrypt base64-encodes. */
std::vector<std::uint8_t> aes_256_gcm_encrypt(std::span<const std::uint8_t, 32> key,
                                              std::span<const std::uint8_t, 12> nonce,
                                              std::string_view plaintext,
                                              std::span<const std::uint8_t> aad = {});

/** Decrypts ciphertext || tag; nullopt on any authentication failure (Kotlin's "Tag mismatch!"). */
std::optional<std::vector<std::uint8_t>> aes_256_gcm_decrypt(
    std::span<const std::uint8_t, 32> key, std::span<const std::uint8_t, 12> nonce,
    std::span<const std::uint8_t> ciphertext_and_tag, std::span<const std::uint8_t> aad = {});

}  // namespace agora::core::crypto
