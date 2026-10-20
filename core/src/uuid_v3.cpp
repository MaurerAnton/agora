// java.util.UUID.nameUUIDFromBytes: MD5 name-based UUID, version 3.
//
// The implementation is RFC 1321 plus the JDK's version/variant nibble overwrite. It lives in its
// own file because it has two callers (custom provider ids and deterministic legacy Run ids);
// SHA-256 stays in sha256.cpp for the streaming fingerprint users.
#include "agora/core/uuid_v3.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace agora::core {
namespace {

constexpr std::uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr std::uint32_t kMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline std::uint32_t rotl(std::uint32_t value, std::uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

void md5_block(std::array<std::uint32_t, 4>& state, const std::uint8_t* block) {
    std::uint32_t words[16];
    for (int i = 0; i < 16; ++i) {
        words[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];

    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f = f + a + kMd5K[i] + words[g];
        a = d;
        d = c;
        c = b;
        b = b + rotl(f, kMd5Shift[i]);
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

std::array<std::uint8_t, 16> md5(std::string_view data) {
    std::array<std::uint32_t, 4> state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};

    const std::size_t full_blocks = data.size() / 64;
    for (std::size_t block = 0; block < full_blocks; ++block) {
        md5_block(state, reinterpret_cast<const std::uint8_t*>(data.data()) + block * 64);
    }

    // Tail: the remaining bytes, a 0x80 marker, zero padding to 56 mod 64, then the bit length.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = data.size() - full_blocks * 64;
    for (std::size_t i = 0; i < rest; ++i) {
        tail[i] = static_cast<std::uint8_t>(data[full_blocks * 64 + i]);
    }
    tail[rest] = 0x80;
    const std::size_t tail_size = rest + 1 <= 56 ? 64 : 128;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[tail_size - 8 + i] = static_cast<std::uint8_t>((bit_length >> (8 * i)) & 0xff);
    }
    md5_block(state, tail.data());
    if (tail_size == 128) md5_block(state, tail.data() + 64);

    std::array<std::uint8_t, 16> digest{};
    for (int i = 0; i < 4; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state[i] & 0xff);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 8) & 0xff);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 16) & 0xff);
        digest[i * 4 + 3] = static_cast<std::uint8_t>((state[i] >> 24) & 0xff);
    }
    return digest;
}

std::string format_uuid(const std::array<std::uint8_t, 16>& bytes) {
    constexpr char kHexLower[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHexLower[bytes[i] >> 4]);
        out.push_back(kHexLower[bytes[i] & 0x0f]);
    }
    return out;
}

}  // namespace

std::string uuid_v3(std::string_view name) {
    std::array<std::uint8_t, 16> digest = md5(name);
    digest[6] = static_cast<std::uint8_t>((digest[6] & 0x0f) | 0x30);
    digest[8] = static_cast<std::uint8_t>((digest[8] & 0x3f) | 0x80);
    return format_uuid(digest);
}

}  // namespace agora::core
