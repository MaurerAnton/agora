#pragma once

#include <string>
#include <string_view>

#include "agora/core/java_utf8.hpp"

namespace agora::core::utf16 {

/**
 * Convert UTF-8 string to UTF-16 using JDK REPLACE-mode decoding (malformed → U+FFFD).
 * Mirrors `String(bytes, StandardCharsets.UTF_8)` in Java/Kotlin.
 */
inline std::u16string from_utf8(std::string_view utf8) {
    std::u16string out;
    out.reserve(utf8.size());
    java_utf8::decode_replace(utf8, out);
    return out;
}

/**
 * Convert UTF-16 string to UTF-8. Valid UTF-16 (including surrogate pairs) is encoded.
 * Invalid surrogates are replaced with U+FFFD per WTF-8 style.
 */
inline std::string to_utf8(std::u16string_view utf16) {
    std::string out;
    out.reserve(utf16.size() * 3);
    for (std::size_t i = 0; i < utf16.size(); ++i) {
        char16_t c = utf16[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c >= 0xD800 && c <= 0xDBFF) {
            // High surrogate - check for low surrogate
            if (i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 && utf16[i + 1] <= 0xDFFF) {
                std::uint32_t code_point = 0x10000 +
                    ((static_cast<std::uint32_t>(c) - 0xD800) << 10) +
                    (static_cast<std::uint32_t>(utf16[i + 1]) - 0xDC00);
                out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
                ++i;
            } else {
                // Unpaired high surrogate
                out.append("\xEF\xBF\xBD");
            }
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            // Unpaired low surrogate
            out.append("\xEF\xBF\xBD");
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

}  // namespace agora::core::utf16