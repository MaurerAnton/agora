#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "agora/core/unicode.hpp"

// Internal UTF-8 <-> UTF-16 bridge for the JDK-faithful ports (java_uri, citations).
// Java's String is a UTF-16 sequence, so every index, slice and length in java.net.URI and in
// the citation policy is expressed in UTF-16 code units; these helpers move between that view
// and the UTF-8 std::string boundary.
namespace agora::core::utf16 {

/** Valid UTF-8 -> UTF-16. A malformed byte becomes U+FFFD (unreachable for app input). */
inline std::u16string from_utf8(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    const char* data = text.data();
    const std::size_t size = text.size();
    std::size_t offset = 0;
    while (offset < size) {
        const unicode::Decoded decoded = unicode::decode_utf8(data + offset, size - offset);
        offset += decoded.length;
        std::uint32_t code_point = decoded.code_point;
        if (decoded.length == 1 && code_point >= 0x80) {
            code_point = 0xFFFD;  // malformed byte
        }
        if (code_point <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(code_point));
        } else {
            code_point -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FF)));
        }
    }
    return out;
}

/** UTF-16 -> UTF-8. An unpaired surrogate becomes '?' exactly like the JVM UTF-8 encoder. */
inline std::string to_utf8(const std::u16string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const char16_t unit = text[i];
        if (unit < 0x80) {
            out.push_back(static_cast<char>(unit));
            ++i;
            continue;
        }
        if (unit < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
            out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
            ++i;
            continue;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] <= 0xDFFF) {
            const std::uint32_t code_point =
                0x10000 + ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
                (static_cast<std::uint32_t>(text[i + 1]) - 0xDC00);
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            i += 2;
            continue;
        }
        if (unit >= 0xD800 && unit <= 0xDFFF) {
            out.push_back('?');
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
        out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
        ++i;
    }
    return out;
}

/** Kotlin String.indexOf(String[, from]): UTF-16 unit index, no case folding. */
inline std::size_t index_of(const std::u16string& haystack, const std::u16string& needle,
                            std::size_t from) {
    if (from > haystack.size()) return std::u16string::npos;
    if (needle.empty()) return from;
    if (needle.size() > haystack.size()) return std::u16string::npos;
    const std::u16string_view view(haystack);
    for (std::size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        if (view.substr(i, needle.size()) == needle) return i;
    }
    return std::u16string::npos;
}

}  // namespace agora::core::utf16
