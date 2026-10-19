#include "agora/core/cjk_text.hpp"

#include <cstddef>
#include <cstdint>

#include "agora/core/unicode.hpp"

namespace agora::core::cjk_text {
namespace {

/**
 * Extension A, the main unified block and the compatibility block.
 * Hiragana/katakana/hangul are excluded: only Han ideographs trigger.
 */
bool is_han(std::uint32_t code_point) {
    return (code_point >= 0x3400 && code_point <= 0x4DBF) ||
           (code_point >= 0x4E00 && code_point <= 0x9FFF) ||
           (code_point >= 0xF900 && code_point <= 0xFAFF);
}

}  // namespace

bool contains_cjk(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unicode::Decoded decoded =
            unicode::decode_utf8(text.data() + offset, text.size() - offset);
        if (is_han(decoded.code_point)) return true;
        if (decoded.length == 0) break;  // defensive: malformed input always advances by one
        offset += decoded.length;
    }
    return false;
}

}  // namespace agora::core::cjk_text
