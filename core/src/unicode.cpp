#include "agora/core/unicode.hpp"

#include "unicode_tables.generated.hpp"

namespace agora::core::unicode {

std::uint8_t flags_of(std::uint32_t code_point) {
    if (code_point >= 0x110000) return 0;
    const auto page = kPageIndex[code_point >> 8];
    return kPages[page * 256 + (code_point & 0xFF)];
}

namespace {

// Byte-length prefix table for UTF-8 lead bytes.
constexpr unsigned lead_length(std::uint8_t byte) {
    if ((byte & 0x80) == 0x00) return 1;
    if ((byte & 0xE0) == 0xC0) return 2;
    if ((byte & 0xF0) == 0xE0) return 3;
    if ((byte & 0xF8) == 0xF0) return 4;
    return 1;  // continuation or invalid lead: treated as one raw byte
}

constexpr bool is_continuation(std::uint8_t byte) { return (byte & 0xC0) == 0x80; }

}  // namespace

Decoded decode_utf8(const char* data, std::size_t size) {
    const auto lead = static_cast<std::uint8_t>(data[0]);
    const unsigned length = lead_length(lead);
    if (length == 1 || size < length) {
        // ASCII, or a truncated/malformed sequence: keep the raw byte as the code point.
        return {lead, 1};
    }
    for (unsigned i = 1; i < length; ++i) {
        if (!is_continuation(static_cast<std::uint8_t>(data[i]))) return {lead, 1};
    }
    std::uint32_t code_point = 0;
    switch (length) {
        case 2:
            code_point = ((lead & 0x1F) << 6) | (data[1] & 0x3F);
            break;
        case 3:
            code_point = ((lead & 0x0F) << 12) | ((data[1] & 0x3F) << 6) | (data[2] & 0x3F);
            break;
        default:
            code_point = ((lead & 0x07) << 18) | ((data[1] & 0x3F) << 12) |
                          ((data[2] & 0x3F) << 6) | (data[3] & 0x3F);
            break;
    }
    return {code_point, length};
}

std::size_t previous_offset(const char* data, std::size_t offset) {
    std::size_t at = offset;
    while (at > 0 && is_continuation(static_cast<std::uint8_t>(data[at - 1]))) at--;
    return at > 0 ? at - 1 : 0;
}

}  // namespace agora::core::unicode
