#include "agora/core/cjk_text.hpp"

#include <cstddef>
#include <cstdint>

#include "agora/core/unicode.hpp"

namespace agora::core::cjk_text {
namespace {

/**
 * The allowed output set of model/CjkText.kt (issue #116 B2): Latin with its extended
 * blocks and ligatures, basic Cyrillic with its supplement, Arabic with its presentation
 * forms, the combining marks of those scripts, and Unicode's Common blocks — digits,
 * punctuation, arrows, math, technical and pictographic symbols, emoji. Everything else
 * (Han, kana, hangul, CJK punctuation, Devanagari, archaic vyaz, Greek, ...) triggers.
 */
bool is_allowed(std::uint32_t code_point) {
    struct Range {
        std::uint32_t first;
        std::uint32_t last;
    };
    static constexpr Range kAllowed[] = {
        {0x0020, 0x007E},    // ASCII letters, digits, punctuation and symbols
        {0x00A0, 0x02FF},    // Latin-1 supplement through Latin Ext-B, IPA, modifiers
        {0x0300, 0x036F},    // combining diacritics shared by Latin and Cyrillic
        {0x0400, 0x04FF},    // basic Cyrillic
        {0x0500, 0x052F},    // Cyrillic Supplement
        {0x0600, 0x06FF},    // Arabic
        {0x0750, 0x077F},    // Arabic Supplement
        {0x08A0, 0x08FF},    // Arabic Extended-A
        {0x1E00, 0x1EFF},    // Latin Extended Additional (Vietnamese)
        {0x2000, 0x206F},    // General Punctuation
        {0x2070, 0x209F},    // superscripts and subscripts
        {0x20A0, 0x20CF},    // currency symbols
        {0x2100, 0x218F},    // letterlike symbols and number forms
        {0x2190, 0x21FF},    // arrows
        {0x2200, 0x22FF},    // mathematical operators
        {0x2300, 0x23FF},    // miscellaneous technical
        {0x2400, 0x24FF},    // control pictures and enclosed alphanumerics
        {0x2500, 0x25FF},    // box drawing, block elements, geometric shapes
        {0x2600, 0x27BF},    // miscellaneous symbols and dingbats
        {0x2C60, 0x2C7F},    // Latin Extended-C
        {0xA720, 0xA7FF},    // Latin Extended-D
        {0xFB00, 0xFB0F},    // Latin ligatures
        {0xFB50, 0xFDFF},    // Arabic Presentation Forms-A
        {0xFE70, 0xFEFF},    // Arabic Presentation Forms-B
        {0xFE00, 0xFE0F},    // variation selectors (emoji sequences)
        {0x1D400, 0x1D7FF},  // Mathematical Alphanumeric Symbols
        {0x1F000, 0x1FAFF},  // emoji and pictographs
    };
    for (const Range& range : kAllowed) {
        if (code_point >= range.first && code_point <= range.last) return true;
    }
    return false;
}

}  // namespace

bool contains_cjk(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unicode::Decoded decoded =
            unicode::decode_utf8(text.data() + offset, text.size() - offset);
        if (decoded.length == 0) break;  // defensive: malformed input never advances
        if (!is_allowed(decoded.code_point)) return true;
        offset += decoded.length;
    }
    return false;
}

}  // namespace agora::core::cjk_text
