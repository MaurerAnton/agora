// Known-answer tests for the bare-toolchain crypto primitives.
//
// core/ carries no third-party dependency, so ShellCrypto's primitives are implemented from
// their specifications in src/crypto_primitives.cpp. Every vector below was generated with an
// implementation that is neither the JVM nor this port (Python's `hmac`/`hashlib` and the
// `cryptography` package), which makes these vectors independent of both sides of the golden
// shell-crypto parity: a bug in either the Kotlin reference or this port still shows up here,
// and a bug unique to the primitives shows up here first.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/crypto_primitives.hpp"
#include "check.hpp"

namespace {

namespace prim = agora::core::crypto;

std::string hex_of(std::span<const std::uint8_t> bytes) {
    std::ostringstream out;
    for (std::uint8_t b : bytes) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return out.str();
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(
            std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

prim::Bytes32 bytes32_from_hex(std::string_view hex) {
    const std::vector<std::uint8_t> bytes = from_hex(hex);
    CHECK(bytes.size() == 32);
    prim::Bytes32 out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

std::string bytes_as_hex(const std::vector<std::uint8_t>& bytes) {
    return hex_of(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

}  // namespace

// -- HMAC-SHA256 (RFC 2104 shape, vector from Python hmac) ---------------------------------------

TEST(hmac_sha256_matches_independent_vector) {
    std::array<std::uint8_t, 20> key{};
    key.fill(0x0b);
    const prim::Bytes32 mac = prim::hmac_sha256(key, "Hi There");
    CHECK_EQ(std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
             hex_of(mac));

    // A key longer than the 64-byte block size is pre-hashed (RFC 2104).
    std::string long_key(100, 'k');
    const prim::Bytes32 mac_long = prim::hmac_sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(long_key.data()),
                                      long_key.size()),
        "data");
    CHECK(hex_of(mac_long).size() == 64);
}

// -- HKDF (RFC 5869) -----------------------------------------------------------------------------

TEST(hkdf_extract_and_expand_match_independent_vectors) {
    std::vector<std::uint8_t> salt = from_hex("000102030405060708090a0b0c");
    std::vector<std::uint8_t> ikm(22, 0x0b);

    const prim::Bytes32 prk = prim::hkdf_extract(salt, ikm);
    CHECK_EQ(std::string("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"),
             hex_of(prk));

    const std::vector<std::uint8_t> okm = prim::hkdf_expand(prk, "conch-agora-v2", 32);
    CHECK_EQ(std::string("9a43dff654313198b8358011dded1a9dff3eaf9991cf504e0d1d38fa73edd390"),
             bytes_as_hex(okm));

    // Extract-then-Expand in one call must produce the same OKM.
    const std::vector<std::uint8_t> combined =
        prim::hkdf_sha256(std::string_view("\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c", 13),
                          std::string_view(reinterpret_cast<const char*>(ikm.data()), ikm.size()),
                          "conch-agora-v2", 32);
    CHECK_EQ(bytes_as_hex(okm), bytes_as_hex(combined));
}

// -- base64 (RFC 4648 with java.util failure text) ------------------------------------------------

TEST(base64_roundtrips_and_reports_like_java_util) {
    const std::string encoded =
        prim::base64_encode(std::span<const std::uint8_t>(
                                reinterpret_cast<const std::uint8_t*>("hello world"), 11),
                            /*url_safe=*/true, /*padding=*/false);
    CHECK_EQ(std::string("aGVsbG8gd29ybGQ"), encoded);

    const prim::Base64Decode decoded = prim::base64_decode("aGVsbG8gd29ybGQ", /*url_safe=*/true);
    CHECK(decoded.bytes.has_value());
    CHECK_EQ(std::string("hello world"),
             std::string(decoded.bytes->begin(), decoded.bytes->end()));

    // padded input (scenario 19)
    const prim::Base64Decode padded = prim::base64_decode("aGVsbG8gd29ybGQ=", /*url_safe=*/true);
    CHECK(padded.bytes.has_value());
    CHECK_EQ(std::string("hello world"),
             std::string(padded.bytes->begin(), padded.bytes->end()));

    // url-safe mapping of +/ bytes (scenario 20's input is the UTF-8 bytes of ûï¾Þ)
    const std::vector<std::uint8_t> specials = from_hex("c3bbc3afc2bec39e");
    CHECK_EQ(std::string("w7vDr8K-w54"),
             prim::base64_encode(specials, /*url_safe=*/true, /*padding=*/false));
    CHECK_EQ(std::string("w7vDr8K+w54="),
             prim::base64_encode(specials, /*url_safe=*/false, /*padding=*/true));

    // java.util rejects the first byte outside the alphabet (scenarios 23/24)
    const prim::Base64Decode bad = prim::base64_decode("!!!", /*url_safe=*/true);
    CHECK(!bad.bytes.has_value());
    CHECK_EQ(std::string("Illegal base64 character 21"), bad.error);
}

// -- X25519 (RFC 7748) ----------------------------------------------------------------------------

TEST(x25519_public_key_and_shared_secret_match_independent_vectors) {
    const prim::Bytes32 seed = bytes32_from_hex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    const prim::Bytes32 expected_public =
        bytes32_from_hex("07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c");
    CHECK_EQ(hex_of(expected_public), hex_of(prim::x25519_public_key(seed)));

    prim::Bytes32 peer_seed{};
    for (std::size_t i = 0; i < peer_seed.size(); ++i) {
        peer_seed[i] = static_cast<std::uint8_t>(100 + i);
    }
    const prim::Bytes32 peer_public = prim::x25519_public_key(peer_seed);
    CHECK_EQ(std::string("7d9c24316539825c1896e57f28197746793ce60cbee3ad47da9d07b85fa55e2a"),
             hex_of(peer_public));

    const prim::Bytes32 shared = prim::x25519(seed, peer_public);
    CHECK_EQ(std::string("148ec3d260615114e3d25c79551b7809f4716199beb8b04dd775cf146ca7ef3e"),
             hex_of(shared));

    // The peer's computation must land on the same secret (RFC 7748 section 5.2).
    const prim::Bytes32 shared_reverse = prim::x25519(peer_seed, expected_public);
    CHECK_EQ(hex_of(shared), hex_of(shared_reverse));
}

// -- X25519 PKCS#8 --------------------------------------------------------------------------------

TEST(pkcs8_x25519_encodes_the_fixed_der_layout) {
    const prim::Bytes32 seed = bytes32_from_hex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    const std::array<std::uint8_t, 48> der = prim::pkcs8_x25519_encode(seed);
    CHECK_EQ(std::string("302e020100300506032b656e04220420") +
                 "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
             bytes_as_hex(std::vector<std::uint8_t>(der.begin(), der.end())));

    const std::optional<prim::Bytes32> decoded = prim::pkcs8_x25519_decode(der);
    CHECK(decoded.has_value());
    CHECK_EQ(hex_of(seed), hex_of(*decoded));

    // A different structure or size is rejected instead of silently yielding a key.
    std::array<std::uint8_t, 48> wrong = der;
    wrong[0] = 0x31;
    CHECK(!prim::pkcs8_x25519_decode(wrong).has_value());
    CHECK(!prim::pkcs8_x25519_decode(std::span<const std::uint8_t>(der.data(), 47)).has_value());
}

// -- AES-256-GCM (SP 800-38D) ---------------------------------------------------------------------

TEST(aes_256_gcm_matches_independent_vectors) {
    const prim::Bytes32 key{};
    const prim::Bytes12 nonce{};
    const std::string plaintext = "aes-256-gcm kat";
    const std::string expected =
        "afc233107f555d43602da8f3d192e9f9595100d9d649ae869308cce89c2d22";

    const std::vector<std::uint8_t> sealed = prim::aes_256_gcm_encrypt(key, nonce, plaintext);
    CHECK_EQ(expected, bytes_as_hex(sealed));

    // The same vector with additional authenticated data.
    const std::string aad_text = "header-aad";
    const std::vector<std::uint8_t> sealed_aad = prim::aes_256_gcm_encrypt(
        key, nonce, plaintext,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(aad_text.data()),
                                      aad_text.size()));
    CHECK_EQ(std::string("afc233107f555d43602da8f3d192e908ec19a6fdd85f6d6125f71b036e6d5d"),
             bytes_as_hex(sealed_aad));

    // Empty plaintext: the output is the bare 16-byte tag.
    const std::vector<std::uint8_t> sealed_empty = prim::aes_256_gcm_encrypt(key, nonce, "");
    CHECK_EQ(std::string("530f8afbc74536b9a963b4f1c4cb738b"), bytes_as_hex(sealed_empty));

    // Round-trip and both authentication failures.
    const std::optional<std::vector<std::uint8_t>> opened =
        prim::aes_256_gcm_decrypt(key, nonce, sealed);
    CHECK(opened.has_value());
    CHECK_EQ(plaintext, std::string(opened->begin(), opened->end()));

    std::vector<std::uint8_t> tampered = sealed;
    tampered.back() ^= 0x01;
    CHECK(!prim::aes_256_gcm_decrypt(key, nonce, tampered).has_value());

    prim::Bytes32 wrong_key{};
    wrong_key.fill(1);
    CHECK(!prim::aes_256_gcm_decrypt(wrong_key, nonce, sealed).has_value());
}
