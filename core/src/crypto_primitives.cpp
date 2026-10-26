#include "agora/core/crypto_primitives.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

#include "agora/core/sha256.hpp"

namespace agora::core::crypto {

namespace {

// ---------------------------------------------------------------------------------------------
// HMAC-SHA256 (RFC 2104) and HKDF (RFC 5869)
// ---------------------------------------------------------------------------------------------

constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kHashSize = 32;

Bytes32 digest(const std::uint8_t* data, std::size_t size) {
    Sha256Digest digest_state;
    digest_state.update(data, size);
    return digest_state.finish();
}

}  // namespace

Bytes32 hmac_sha256(std::span<const std::uint8_t> key, std::string_view data) {
    std::array<std::uint8_t, kBlockSize> block{};
    if (key.size() > kBlockSize) {
        const Bytes32 hashed = digest(key.data(), key.size());
        std::copy(hashed.begin(), hashed.end(), block.begin());
    } else {
        std::copy(key.begin(), key.end(), block.begin());
    }

    std::array<std::uint8_t, kBlockSize> inner_key{};
    std::array<std::uint8_t, kBlockSize> outer_key{};
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        inner_key[i] = static_cast<std::uint8_t>(block[i] ^ 0x36);
        outer_key[i] = static_cast<std::uint8_t>(block[i] ^ 0x5c);
    }

    Sha256Digest inner;
    inner.update(inner_key.data(), inner_key.size());
    inner.update(data);
    const Bytes32 inner_hash = inner.finish();

    Sha256Digest outer;
    outer.update(outer_key.data(), outer_key.size());
    outer.update(inner_hash.data(), inner_hash.size());
    return outer.finish();
}

Bytes32 hkdf_extract(std::span<const std::uint8_t> salt, std::span<const std::uint8_t> ikm) {
    return hmac_sha256(salt, std::string_view(reinterpret_cast<const char*>(ikm.data()),
                                              ikm.size()));
}

std::vector<std::uint8_t> hkdf_expand(std::span<const std::uint8_t> prk, std::string_view info,
                                      std::size_t length) {
    std::vector<std::uint8_t> result;
    result.reserve(length);
    Bytes32 previous{};
    std::size_t previous_len = 0;
    std::uint8_t counter = 1;
    while (result.size() < length) {
        std::string block_input;
        block_input.reserve(previous_len + info.size() + 1);
        block_input.append(reinterpret_cast<const char*>(previous.data()), previous_len);
        block_input.append(info);
        block_input.push_back(static_cast<char>(counter++));
        const Bytes32 block = hmac_sha256(prk, block_input);
        previous = block;
        previous_len = block.size();
        const std::size_t want = std::min(previous_len, length - result.size());
        result.insert(result.end(), previous.begin(),
                      previous.begin() + static_cast<std::ptrdiff_t>(want));
    }
    return result;
}

std::vector<std::uint8_t> hkdf_sha256(std::string_view salt, std::string_view ikm,
                                      std::string_view info, std::size_t length) {
    // RFC 5869: an absent salt is a HashLen string of zeros. HMAC pads an empty key and a
    // zero-filled HashLen key to the same ipad/opad, so either spelling derives the same PRK;
    // the explicit buffer keeps ShellCrypto.deriveAesKey's ByteArray(32) shape visible here.
    const std::array<std::uint8_t, 32> zero{};
    const auto* salt_data = reinterpret_cast<const std::uint8_t*>(salt.data());
    std::size_t salt_size = salt.size();
    if (salt.empty()) {
        salt_data = zero.data();
        salt_size = zero.size();
    }
    const Bytes32 prk = hkdf_extract(
        std::span<const std::uint8_t>(salt_data, salt_size),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(ikm.data()),
                                      ikm.size()));
    return hkdf_expand(prk, info, length);
}

// ---------------------------------------------------------------------------------------------
// base64 (RFC 4648) with java.util.Base64.Decoder failure descriptions
// ---------------------------------------------------------------------------------------------

std::string base64_encode(std::span<const std::uint8_t> input, bool url_safe, bool padding) {
    static constexpr char kStandard[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static constexpr char kUrl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* table = url_safe ? kUrl : kStandard;

    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= input.size(); i += 3) {
        const std::uint32_t group = (static_cast<std::uint32_t>(input[i]) << 16) |
                                    (static_cast<std::uint32_t>(input[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(input[i + 2]);
        out.push_back(table[(group >> 18) & 0x3F]);
        out.push_back(table[(group >> 12) & 0x3F]);
        out.push_back(table[(group >> 6) & 0x3F]);
        out.push_back(table[group & 0x3F]);
    }
    const std::size_t rest = input.size() - i;
    if (rest == 1) {
        const std::uint32_t group = static_cast<std::uint32_t>(input[i]) << 16;
        out.push_back(table[(group >> 18) & 0x3F]);
        out.push_back(table[(group >> 12) & 0x3F]);
        if (padding) out.append(2, '=');
    } else if (rest == 2) {
        const std::uint32_t group = (static_cast<std::uint32_t>(input[i]) << 16) |
                                    (static_cast<std::uint32_t>(input[i + 1]) << 8);
        out.push_back(table[(group >> 18) & 0x3F]);
        out.push_back(table[(group >> 12) & 0x3F]);
        out.push_back(table[(group >> 6) & 0x3F]);
        if (padding) out.push_back('=');
    }
    return out;
}

namespace {

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string illegal_char_error(char c) {
    std::ostringstream oss;
    oss << "Illegal base64 character " << std::hex
        << static_cast<int>(static_cast<unsigned char>(c));
    return oss.str();
}

}  // namespace

Base64Decode base64_decode(std::string_view input, bool url_safe) {
    // platform/Base64.java: URL_SAFE maps -_ to +/ first, then appends the missing padding,
    // so java.util.Base64.Decoder always scans a whole number of four-character quanta.
    std::string mapped(input);
    if (url_safe) {
        std::replace(mapped.begin(), mapped.end(), '-', '+');
        std::replace(mapped.begin(), mapped.end(), '_', '/');
    }
    mapped.append((4 - mapped.size() % 4) % 4, '=');

    std::vector<std::uint8_t> out;
    out.reserve(mapped.size() / 4 * 3);
    bool pads_finished = false;
    for (std::size_t i = 0; i < mapped.size(); i += 4) {
        int values[4] = {0, 0, 0, 0};
        bool pad[4] = {false, false, false, false};
        for (int k = 0; k < 4; ++k) {
            const char c = mapped[i + static_cast<std::size_t>(k)];
            if (c == '=') {
                pad[k] = true;
                continue;
            }
            if (pads_finished) {
                // data after padding -- java reports the offending byte against '=' state
                return {std::nullopt, illegal_char_error('=')};
            }
            const int value = base64_value(c);
            if (value < 0) return {std::nullopt, illegal_char_error(c)};
            values[k] = value;
        }
        if (pad[0] || pad[1]) {
            // "=..." or "X=..": a pad where the first or second data character belongs
            return {std::nullopt, illegal_char_error('=')};
        }
        if (pad[2] && !pad[3]) {
            // "XX=Y"
            return {std::nullopt, illegal_char_error('=')};
        }

        if (pad[3] && !pad[2]) {
            // "XXX=" -> two bytes
            const std::uint32_t group = (static_cast<std::uint32_t>(values[0]) << 18) |
                                        (static_cast<std::uint32_t>(values[1]) << 12) |
                                        (static_cast<std::uint32_t>(values[2]) << 6);
            out.push_back(static_cast<std::uint8_t>(group >> 16));
            out.push_back(static_cast<std::uint8_t>(group >> 8));
            pads_finished = true;
        } else if (pad[2] && pad[3]) {
            // "XX==" -> one byte
            const std::uint32_t group = (static_cast<std::uint32_t>(values[0]) << 18) |
                                        (static_cast<std::uint32_t>(values[1]) << 12);
            out.push_back(static_cast<std::uint8_t>(group >> 16));
            pads_finished = true;
        } else {
            const std::uint32_t group =
                (static_cast<std::uint32_t>(values[0]) << 18) |
                (static_cast<std::uint32_t>(values[1]) << 12) |
                (static_cast<std::uint32_t>(values[2]) << 6) | static_cast<std::uint32_t>(values[3]);
            out.push_back(static_cast<std::uint8_t>(group >> 16));
            out.push_back(static_cast<std::uint8_t>(group >> 8));
            out.push_back(static_cast<std::uint8_t>(group));
        }
    }
    return {out, std::string{}};
}

// ---------------------------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------------------------

void random_bytes(std::span<std::uint8_t> out) {
    std::random_device device;
    for (std::uint8_t& byte : out) {
        byte = static_cast<std::uint8_t>(device());
    }
}

// ---------------------------------------------------------------------------------------------
// X25519 (RFC 7748): field arithmetic over GF(2^255-19) in 51-bit limbs
// ---------------------------------------------------------------------------------------------

namespace {

using u128 = unsigned __int128;

constexpr std::uint64_t kMask51 = 0x7ffffffffffffULL;
// p = 2^255 - 19, in five 51-bit limbs (limb0 = 2^51 - 19)
constexpr std::uint64_t kP[5] = {0x7ffffffffffedULL, kMask51, kMask51, kMask51, kMask51};

struct Fe {
    std::uint64_t v[5];
};

Fe fe_zero() {
    return Fe{0, 0, 0, 0, 0};
}

Fe fe_one() {
    return Fe{1, 0, 0, 0, 0};
}

std::uint64_t load64_le(const std::uint8_t* s) {
    std::uint64_t r = 0;
    for (int i = 7; i >= 0; --i) r = (r << 8) | s[i];
    return r;
}

Fe fe_canon(u128 r[5]);

Fe fe_frombytes(const std::uint8_t s[32]) {
    Fe h;
    h.v[0] = load64_le(s + 0) & kMask51;
    h.v[1] = (load64_le(s + 6) >> 3) & kMask51;
    h.v[2] = (load64_le(s + 12) >> 6) & kMask51;
    h.v[3] = (load64_le(s + 19) >> 1) & kMask51;
    h.v[4] = (load64_le(s + 24) >> 12) & kMask51;
    // RFC 7748 tells callers to mask bit255, which can leave a value in [p, p+19); reduce so
    // the every-element-canonical invariant holds for the ladder's subtrahends.
    u128 r[5] = {h.v[0], h.v[1], h.v[2], h.v[3], h.v[4]};
    return fe_canon(r);
}

// Reduces any five-limb representative to the canonical one in [0, p): carries to 51-bit limbs
// (folding limb4's overflow with the factor 19), then subtracts p exactly when the value is
// still >= p, detected by whether value + 19 overflows 2^255 (2^255 = p + 19).
Fe fe_canon(u128 r[5]) {
    std::uint64_t t[5];
    std::uint64_t c = static_cast<std::uint64_t>(r[0] >> 51);
    t[0] = static_cast<std::uint64_t>(r[0]) & kMask51;
    r[1] += c;
    c = static_cast<std::uint64_t>(r[1] >> 51);
    t[1] = static_cast<std::uint64_t>(r[1]) & kMask51;
    r[2] += c;
    c = static_cast<std::uint64_t>(r[2] >> 51);
    t[2] = static_cast<std::uint64_t>(r[2]) & kMask51;
    r[3] += c;
    c = static_cast<std::uint64_t>(r[3] >> 51);
    t[3] = static_cast<std::uint64_t>(r[3]) & kMask51;
    r[4] += c;
    c = static_cast<std::uint64_t>(r[4] >> 51);
    t[4] = static_cast<std::uint64_t>(r[4]) & kMask51;
    t[0] += 19 * c;

    // The fold above can re-overflow limb0 by a hair; repeat the pass until it settles. Each
    // pass preserves the represented value.
    for (int pass = 0; pass < 8; ++pass) {
        bool carried = false;
        c = t[0] >> 51;
        if (c) {
            carried = true;
            t[0] &= kMask51;
            t[1] += c;
        }
        for (int i = 1; i < 4; ++i) {
            c = t[i] >> 51;
            if (c) {
                carried = true;
                t[i] &= kMask51;
                t[i + 1] += c;
            }
        }
        c = t[4] >> 51;
        if (c) {
            carried = true;
            t[4] &= kMask51;
            t[0] += 19 * c;
        }
        if (!carried) break;
    }

    // value + 19: chain the addition to learn whether bit255 (i.e. p) is reached.
    std::uint64_t a[5];
    std::uint64_t carry = t[0] + 19;
    a[0] = carry & kMask51;
    carry >>= 51;
    for (int i = 1; i < 5; ++i) {
        carry += t[i];
        a[i] = carry & kMask51;
        carry >>= 51;
    }
    const std::uint64_t* out = carry ? a : t;
    Fe result;
    for (int i = 0; i < 5; ++i) result.v[i] = out[i];
    return result;
}

Fe fe_add(const Fe& x, const Fe& y) {
    u128 r[5];
    u128 carry = 0;
    for (int i = 0; i < 4; ++i) {
        r[i] = static_cast<u128>(x.v[i]) + y.v[i] + carry;
        carry = r[i] >> 51;
        r[i] &= kMask51;
    }
    // limb4 keeps its raw overflow: fe_canon folds that carry past 2^255 with the factor 19.
    r[4] = static_cast<u128>(x.v[4]) + y.v[4] + carry;
    return fe_canon(r);
}

Fe fe_sub(const Fe& x, const Fe& y) {
    // x - y == x + (p - y); y is canonical, so p - y >= 1 and the limb borrow chain ends at 0.
    u128 diff[5];
    std::uint64_t borrow = 0;
    for (int i = 0; i < 5; ++i) {
        const u128 rhs = static_cast<u128>(y.v[i]) + borrow;
        if (kP[i] >= rhs) {
            diff[i] = kP[i] - rhs;
            borrow = 0;
        } else {
            diff[i] = (static_cast<u128>(1) << 51) + kP[i] - rhs;
            borrow = 1;
        }
    }

    u128 r[5];
    u128 carry = 0;
    for (int i = 0; i < 4; ++i) {
        r[i] = static_cast<u128>(x.v[i]) + diff[i] + carry;
        carry = r[i] >> 51;
        r[i] &= kMask51;
    }
    r[4] = static_cast<u128>(x.v[4]) + diff[4] + carry;
    return fe_canon(r);
}

Fe fe_mul(const Fe& x, const Fe& y) {
    const std::uint64_t a0 = x.v[0], a1 = x.v[1], a2 = x.v[2], a3 = x.v[3], a4 = x.v[4];
    const std::uint64_t b0 = y.v[0], b1 = y.v[1], b2 = y.v[2], b3 = y.v[3], b4 = y.v[4];

    // Products reduce modulo 2^255-19 by folding the term that lands on 2^255 with the factor 19.
    const u128 r0 = static_cast<u128>(a0) * b0 +
                    19 * (static_cast<u128>(a1) * b4 + static_cast<u128>(a2) * b3 +
                          static_cast<u128>(a3) * b2 + static_cast<u128>(a4) * b1);
    const u128 r1 = static_cast<u128>(a0) * b1 + static_cast<u128>(a1) * b0 +
                    19 * (static_cast<u128>(a2) * b4 + static_cast<u128>(a3) * b3 +
                          static_cast<u128>(a4) * b2);
    const u128 r2 = static_cast<u128>(a0) * b2 + static_cast<u128>(a1) * b1 +
                    static_cast<u128>(a2) * b0 +
                    19 * (static_cast<u128>(a3) * b4 + static_cast<u128>(a4) * b3);
    const u128 r3 = static_cast<u128>(a0) * b3 + static_cast<u128>(a1) * b2 +
                    static_cast<u128>(a2) * b1 + static_cast<u128>(a3) * b0 +
                    19 * static_cast<u128>(a4) * b4;
    const u128 r4 = static_cast<u128>(a0) * b4 + static_cast<u128>(a1) * b3 +
                    static_cast<u128>(a2) * b2 + static_cast<u128>(a3) * b1 +
                    static_cast<u128>(a4) * b0;

    u128 r[5] = {r0, r1, r2, r3, r4};
    return fe_canon(r);
}

Fe fe_sq(const Fe& x) {
    return fe_mul(x, x);
}

Fe fe_mul121665(const Fe& x) {
    u128 r[5];
    for (int i = 0; i < 5; ++i) r[i] = static_cast<u128>(x.v[i]) * 121665;
    return fe_canon(r);
}

void fe_cswap(Fe& a, Fe& b, std::uint64_t swap) {
    const std::uint64_t mask = 0 - swap;
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t carried = mask & (a.v[i] ^ b.v[i]);
        a.v[i] ^= carried;
        b.v[i] ^= carried;
    }
}

void fe_tobytes(std::uint8_t s[32], const Fe& h) {
    // h is canonical (< p, bit255 clear); place the five 51-bit limbs little-endian.
    std::memset(s, 0, 32);
    for (int limb = 0; limb < 5; ++limb) {
        const std::uint64_t value = h.v[limb];
        const int base = limb * 51;
        for (int bit = 0; bit < 51; ++bit) {
            if ((value >> bit) & 1) {
                const int pos = base + bit;
                s[pos >> 3] |= static_cast<std::uint8_t>(1u << (pos & 7));
            }
        }
    }
}

Fe fe_invert(const Fe& x) {
    // x^(p-2); p-2 = 2^255 - 21 as little-endian bytes.
    static constexpr std::uint8_t kExponent[32] = {
        0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0x7f};
    Fe r = fe_one();
    for (int i = 254; i >= 0; --i) {
        r = fe_sq(r);
        if ((kExponent[i >> 3] >> (i & 7)) & 1) r = fe_mul(r, x);
    }
    return r;
}

void x25519_scalarmult(std::uint8_t out[32], const std::uint8_t scalar[32],
                       const std::uint8_t point[32]) {
    std::uint8_t k[32];
    std::memcpy(k, scalar, 32);
    // RFC 7748 clamping, exactly what the JDK's X25519 KeyAgreement applies to the seed.
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    const Fe x1 = fe_frombytes(point);
    Fe x2 = fe_one();   // RFC 7748: x_2 = 1
    Fe z2 = fe_zero();  //          z_2 = 0
    Fe x3 = x1;
    Fe z3 = fe_one();
    std::uint64_t swap = 0;

    for (int t = 254; t >= 0; --t) {
        const std::uint64_t kt = (k[t >> 3] >> (t & 7)) & 1;
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;

        const Fe a = fe_add(x2, z2);
        const Fe aa = fe_sq(a);
        const Fe b = fe_sub(x2, z2);
        const Fe bb = fe_sq(b);
        const Fe e = fe_sub(aa, bb);
        const Fe c = fe_add(x3, z3);
        const Fe d = fe_sub(x3, z3);
        const Fe da = fe_mul(d, a);
        const Fe cb = fe_mul(c, b);
        const Fe da_plus_cb = fe_add(da, cb);
        const Fe da_minus_cb = fe_sub(da, cb);
        x3 = fe_sq(da_plus_cb);
        z3 = fe_mul(x1, fe_sq(da_minus_cb));
        x2 = fe_mul(aa, bb);
        z2 = fe_mul(e, fe_add(aa, fe_mul121665(e)));
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    const Fe z2inv = fe_invert(z2);
    fe_tobytes(out, fe_mul(x2, z2inv));
}

}  // namespace

Bytes32 x25519_public_key(std::span<const std::uint8_t, 32> seed) {
    const std::uint8_t basepoint[32] = {9};
    Bytes32 out{};
    x25519_scalarmult(out.data(), seed.data(), basepoint);
    return out;
}

Bytes32 x25519(std::span<const std::uint8_t, 32> seed, std::span<const std::uint8_t, 32> peer_public) {
    Bytes32 out{};
    x25519_scalarmult(out.data(), seed.data(), peer_public.data());
    return out;
}

// ---------------------------------------------------------------------------------------------
// X25519 PKCS#8 (one-private-keyInfo, id-X25519) -- the fixed 48-byte DER layout
// ---------------------------------------------------------------------------------------------

namespace {

// SEQUENCE(46) { INTEGER 0, SEQUENCE { OID 1.3.101.110 }, OCTET STRING(34) { OCTET STRING(32) } }
constexpr std::array<std::uint8_t, 16> kPkcs8Prefix = {0x30, 0x2e, 0x02, 0x01, 0x00, 0x30,
                                                       0x05, 0x06, 0x03, 0x2b, 0x65, 0x6e,
                                                       0x04, 0x22, 0x04, 0x20};

}  // namespace

std::array<std::uint8_t, 48> pkcs8_x25519_encode(std::span<const std::uint8_t, 32> seed) {
    std::array<std::uint8_t, 48> der{};
    std::copy(kPkcs8Prefix.begin(), kPkcs8Prefix.end(), der.begin());
    std::copy(seed.begin(), seed.end(), der.begin() + kPkcs8Prefix.size());
    return der;
}

std::optional<Bytes32> pkcs8_x25519_decode(std::span<const std::uint8_t> der) {
    if (der.size() != 48) return std::nullopt;
    if (!std::equal(kPkcs8Prefix.begin(), kPkcs8Prefix.end(), der.begin())) return std::nullopt;
    Bytes32 seed{};
    std::copy(der.begin() + kPkcs8Prefix.size(), der.end(), seed.begin());
    return seed;
}

// ---------------------------------------------------------------------------------------------
// AES-256 (FIPS 197) + GCM (NIST SP 800-38D)
//
// GCM only ever needs the AES *encryption* direction (CTR mode), so only that is implemented.
// The S-box is generated from the specification (GF(2^8) inverse + affine transform) instead of
// being transcribed, so there is no table-copy step to get wrong; the known-answer vectors in
// tests/crypto_primitives_test.cpp pin the generated table, the key schedule and GCM itself.
// ---------------------------------------------------------------------------------------------

namespace {

// -- GF(2^8) arithmetic (polynomial 0x11b) ------------------------------------------------------

std::uint8_t gf_mul8(std::uint8_t a, std::uint8_t b) {
    std::uint8_t product = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) product ^= a;
        const bool high = (a & 0x80) != 0;
        a = static_cast<std::uint8_t>(a << 1);
        if (high) a ^= 0x1b;
        b = static_cast<std::uint8_t>(b >> 1);
    }
    return product;
}

std::uint8_t gf_pow254(std::uint8_t x) {
    // x^254 is the multiplicative inverse in GF(2^8) (254 = 2^8 - 2).
    std::uint8_t result = 1;
    std::uint8_t base = x;
    int exponent = 254;
    while (exponent > 0) {
        if (exponent & 1) result = gf_mul8(result, base);
        base = gf_mul8(base, base);
        exponent >>= 1;
    }
    return result;
}

std::uint8_t rotl8(std::uint8_t value, int bits) {
    return static_cast<std::uint8_t>((value << bits) | (value >> (8 - bits)));
}

// -- generated AES tables -----------------------------------------------------------------------

struct AesTables {
    std::uint8_t sbox[256];
};

const AesTables& aes_tables() {
    static const AesTables tables = [] {
        AesTables t{};
        for (int i = 0; i < 256; ++i) {
            const std::uint8_t inverse = gf_pow254(static_cast<std::uint8_t>(i));
            t.sbox[i] = static_cast<std::uint8_t>(
                inverse ^ rotl8(inverse, 1) ^ rotl8(inverse, 2) ^ rotl8(inverse, 3) ^
                rotl8(inverse, 4) ^ 0x63);
        }
        return t;
    }();
    return tables;
}

// -- AES-256 key schedule and block encryption --------------------------------------------------

struct AesSchedule {
    // AES-256: 8 key words, 14 rounds -> 60 words = 240 bytes = 15 sixteen-byte round keys
    std::uint8_t round_keys[15][16];
};

AesSchedule aes256_expand(const std::uint8_t key[32]) {
    const AesTables& tables = aes_tables();
    AesSchedule schedule{};
    std::uint32_t w[60];
    for (int i = 0; i < 8; ++i) {
        w[i] = (static_cast<std::uint32_t>(key[4 * i]) << 24) |
               (static_cast<std::uint32_t>(key[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(key[4 * i + 2]) << 8) |
               static_cast<std::uint32_t>(key[4 * i + 3]);
    }
    std::uint32_t rcon = 1;
    for (int i = 8; i < 60; ++i) {
        std::uint32_t temp = w[i - 1];
        if (i % 8 == 0) {
            temp = (temp << 8) | (temp >> 24);  // RotWord
            temp = (static_cast<std::uint32_t>(tables.sbox[(temp >> 24) & 0xff]) << 24) |
                   (static_cast<std::uint32_t>(tables.sbox[(temp >> 16) & 0xff]) << 16) |
                   (static_cast<std::uint32_t>(tables.sbox[(temp >> 8) & 0xff]) << 8) |
                   static_cast<std::uint32_t>(tables.sbox[temp & 0xff]);  // SubWord
            // Rcon applies to the first byte of the word (the MSB of the big-endian word).
            temp ^= rcon << 24;
            rcon = gf_mul8(static_cast<std::uint8_t>(rcon), 2);
        } else if (i % 8 == 4) {
            temp = (static_cast<std::uint32_t>(tables.sbox[(temp >> 24) & 0xff]) << 24) |
                   (static_cast<std::uint32_t>(tables.sbox[(temp >> 16) & 0xff]) << 16) |
                   (static_cast<std::uint32_t>(tables.sbox[(temp >> 8) & 0xff]) << 8) |
                   static_cast<std::uint32_t>(tables.sbox[temp & 0xff]);
        }
        w[i] = w[i - 8] ^ temp;
    }
    for (int round = 0; round < 15; ++round) {
        for (int word = 0; word < 4; ++word) {
            const std::uint32_t value = w[round * 4 + word];
            schedule.round_keys[round][4 * word] = static_cast<std::uint8_t>(value >> 24);
            schedule.round_keys[round][4 * word + 1] = static_cast<std::uint8_t>(value >> 16);
            schedule.round_keys[round][4 * word + 2] = static_cast<std::uint8_t>(value >> 8);
            schedule.round_keys[round][4 * word + 3] = static_cast<std::uint8_t>(value);
        }
    }
    return schedule;
}

void aes256_encrypt_block(const AesSchedule& schedule, const std::uint8_t in[16],
                          std::uint8_t out[16]) {
    const AesTables& tables = aes_tables();
    std::uint8_t state[16];
    std::memcpy(state, in, 16);

    auto add_round_key = [&](int round) {
        for (int i = 0; i < 16; ++i) state[i] ^= schedule.round_keys[round][i];
    };
    auto sub_bytes = [&] {
        for (int i = 0; i < 16; ++i) state[i] = tables.sbox[state[i]];
    };
    auto shift_rows = [&] {
        // state[r + 4c] := state[r + 4((c + r) mod 4)]
        std::uint8_t shifted[16];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                shifted[r + 4 * c] = state[r + 4 * ((c + r) % 4)];
            }
        }
        std::memcpy(state, shifted, 16);
    };
    auto mix_columns = [&] {
        for (int c = 0; c < 4; ++c) {
            const std::uint8_t a0 = state[0 + 4 * c];
            const std::uint8_t a1 = state[1 + 4 * c];
            const std::uint8_t a2 = state[2 + 4 * c];
            const std::uint8_t a3 = state[3 + 4 * c];
            state[0 + 4 * c] = gf_mul8(a0, 2) ^ gf_mul8(a1, 3) ^ a2 ^ a3;
            state[1 + 4 * c] = a0 ^ gf_mul8(a1, 2) ^ gf_mul8(a2, 3) ^ a3;
            state[2 + 4 * c] = a0 ^ a1 ^ gf_mul8(a2, 2) ^ gf_mul8(a3, 3);
            state[3 + 4 * c] = gf_mul8(a0, 3) ^ a1 ^ a2 ^ gf_mul8(a3, 2);
        }
    };

    add_round_key(0);
    for (int round = 1; round < 14; ++round) {
        sub_bytes();
        shift_rows();
        mix_columns();
        add_round_key(round);
    }
    sub_bytes();
    shift_rows();
    add_round_key(14);
    std::memcpy(out, state, 16);
}

// -- GCM (SP 800-38D) ---------------------------------------------------------------------------

void inc32(std::uint8_t counter[16]) {
    for (int i = 15; i >= 12; --i) {
        if (++counter[i] != 0) break;
    }
}

/** GF(2^128) multiplication, SP 800-38D bitwise algorithm (big-endian bit order). */
void gf128_mul(std::uint8_t out[16], const std::uint8_t x[16], const std::uint8_t h[16]) {
    std::uint8_t z[16] = {0};
    std::uint8_t v[16];
    std::memcpy(v, h, 16);
    for (int i = 0; i < 128; ++i) {
        const bool bit = ((x[i >> 3] >> (7 - (i & 7))) & 1) != 0;
        if (bit) {
            for (int j = 0; j < 16; ++j) z[j] ^= v[j];
        }
        const bool lsb = (v[15] & 1) != 0;
        for (int j = 15; j > 0; --j) {
            v[j] = static_cast<std::uint8_t>((v[j] >> 1) | ((v[j - 1] & 1) << 7));
        }
        v[0] = static_cast<std::uint8_t>(v[0] >> 1);
        if (lsb) v[0] ^= 0xe1;
    }
    std::memcpy(out, z, 16);
}

void ghash_block(std::uint8_t y[16], const std::uint8_t h[16], const std::uint8_t block[16]) {
    std::uint8_t xored[16];
    for (int j = 0; j < 16; ++j) xored[j] = y[j] ^ block[j];
    gf128_mul(y, xored, h);
}

/** GHASH over data, zero-padded to a multiple of 16 (one update per padded block). */
void ghash_update(std::uint8_t y[16], const std::uint8_t h[16], const std::uint8_t* data,
                  std::size_t size) {
    std::size_t offset = 0;
    while (offset + 16 <= size) {
        ghash_block(y, h, data + offset);
        offset += 16;
    }
    std::uint8_t tail[16] = {0};
    if (offset < size) {
        std::memcpy(tail, data + offset, size - offset);
        ghash_block(y, h, tail);
    }
}

void gctr(const AesSchedule& schedule, std::uint8_t counter[16], const std::uint8_t* in,
          std::size_t size, std::uint8_t* out) {
    std::size_t offset = 0;
    while (offset < size) {
        std::uint8_t keystream[16];
        aes256_encrypt_block(schedule, counter, keystream);
        inc32(counter);
        const std::size_t take = std::min<std::size_t>(16, size - offset);
        for (std::size_t j = 0; j < take; ++j) {
            out[offset + j] = in[offset + j] ^ keystream[j];
        }
        offset += take;
    }
}

/** Computes the 16-byte GHASH authentication value over aad || pad || ct || pad || lengths. */
void gcm_s(const std::uint8_t h[16], std::span<const std::uint8_t> aad,
           std::span<const std::uint8_t> ciphertext, std::uint8_t out[16]) {
    std::uint8_t y[16] = {0};
    ghash_update(y, h, aad.data(), aad.size());
    ghash_update(y, h, ciphertext.data(), ciphertext.size());
    std::uint8_t lengths[16] = {0};
    const std::uint64_t aad_bits = static_cast<std::uint64_t>(aad.size()) * 8;
    const std::uint64_t ct_bits = static_cast<std::uint64_t>(ciphertext.size()) * 8;
    for (int i = 0; i < 8; ++i) {
        lengths[i] = static_cast<std::uint8_t>(aad_bits >> (56 - 8 * i));
        lengths[8 + i] = static_cast<std::uint8_t>(ct_bits >> (56 - 8 * i));
    }
    ghash_block(y, h, lengths);
    std::memcpy(out, y, 16);
}

/** j0 for the mandatory 96-bit IV: IV || 0^31 || 1. */
void gcm_j0(const std::uint8_t nonce[12], std::uint8_t out[16]) {
    std::memcpy(out, nonce, 12);
    out[12] = 0;
    out[13] = 0;
    out[14] = 0;
    out[15] = 1;
}

void gcm_tag(const AesSchedule& schedule, const std::uint8_t h[16],
             std::span<const std::uint8_t> aad, std::span<const std::uint8_t> ciphertext,
             const std::uint8_t j0[16], std::uint8_t out[16]) {
    std::uint8_t s[16];
    gcm_s(h, aad, ciphertext, s);
    std::uint8_t mask[16];
    aes256_encrypt_block(schedule, j0, mask);
    for (int i = 0; i < 16; ++i) out[i] = s[i] ^ mask[i];
}

}  // namespace

std::vector<std::uint8_t> aes_256_gcm_encrypt(std::span<const std::uint8_t, 32> key,
                                              std::span<const std::uint8_t, 12> nonce,
                                              std::string_view plaintext,
                                              std::span<const std::uint8_t> aad) {
    const AesSchedule schedule = aes256_expand(key.data());

    std::uint8_t zero[16] = {0};
    std::uint8_t h[16];
    aes256_encrypt_block(schedule, zero, h);

    std::uint8_t j0[16];
    gcm_j0(nonce.data(), j0);

    std::uint8_t counter[16];
    std::memcpy(counter, j0, 16);
    inc32(counter);

    std::vector<std::uint8_t> ciphertext(plaintext.size());
    gctr(schedule, counter, reinterpret_cast<const std::uint8_t*>(plaintext.data()),
         plaintext.size(), ciphertext.data());

    std::uint8_t tag[16];
    gcm_tag(schedule, h, aad, ciphertext, j0, tag);

    std::vector<std::uint8_t> out;
    out.reserve(ciphertext.size() + 16);
    out.insert(out.end(), ciphertext.begin(), ciphertext.end());
    out.insert(out.end(), tag, tag + 16);
    return out;
}

std::optional<std::vector<std::uint8_t>> aes_256_gcm_decrypt(
    std::span<const std::uint8_t, 32> key, std::span<const std::uint8_t, 12> nonce,
    std::span<const std::uint8_t> ciphertext_and_tag, std::span<const std::uint8_t> aad) {
    if (ciphertext_and_tag.size() < 16) return std::nullopt;
    const std::size_t ciphertext_size = ciphertext_and_tag.size() - 16;
    const auto* ciphertext = ciphertext_and_tag.data();
    const auto* tag = ciphertext_and_tag.data() + ciphertext_size;

    const AesSchedule schedule = aes256_expand(key.data());

    std::uint8_t zero[16] = {0};
    std::uint8_t h[16];
    aes256_encrypt_block(schedule, zero, h);

    std::uint8_t j0[16];
    gcm_j0(nonce.data(), j0);

    std::uint8_t expected[16];
    gcm_tag(schedule, h, aad,
            std::span<const std::uint8_t>(ciphertext, ciphertext_size), j0, expected);

    // Constant-time tag comparison: any mismatch is an authentication failure.
    std::uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= static_cast<std::uint8_t>(expected[i] ^ tag[i]);
    if (diff != 0) return std::nullopt;

    std::uint8_t counter[16];
    std::memcpy(counter, j0, 16);
    inc32(counter);

    std::vector<std::uint8_t> plaintext(ciphertext_size);
    gctr(schedule, counter, ciphertext, ciphertext_size, plaintext.data());
    return plaintext;
}

}  // namespace agora::core::crypto
