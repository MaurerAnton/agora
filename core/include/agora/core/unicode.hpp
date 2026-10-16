#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace agora::core::unicode {

// Classification bits, one byte per codepoint. The tables are generated from the JVM's own
// Unicode data so that this port classifies characters exactly like the Kotlin implementation
// it replaces (see core/tools/UnicodeTableGen.java).
enum Flag : std::uint8_t {
    kLetter = 1 << 0,        // \p{L}: Lu Ll Lt Lm Lo
    kNumber = 1 << 1,        // \p{N}: Nd Nl No
    kUpperRun = 1 << 2,      // the o200k pattern's leading letter class: Lu Lt Lm Lo M
    kLowerRun = 1 << 3,      // the o200k pattern's trailing letter class: Ll Lm Lo M
    kWhitespace = 1 << 4,    // the o200k pattern's explicit whitespace class
    kJvmWhitespace = 1 << 5, // Character.isWhitespace
    kJvmSpace = 1 << 6,      // Character.isSpaceChar
};

std::uint8_t flags_of(std::uint32_t code_point);

inline bool is_letter(std::uint32_t cp) { return (flags_of(cp) & kLetter) != 0; }
inline bool is_number(std::uint32_t cp) { return (flags_of(cp) & kNumber) != 0; }

/** Kotlin's Char.isWhitespace(): JVM whitespace, then space characters. */
inline bool is_kotlin_whitespace(std::uint32_t cp) {
    const std::uint8_t flags = flags_of(cp);
    return (flags & (kJvmWhitespace | kJvmSpace)) != 0;
}

/** Java's Character.isWhitespace(): space separators except the non-breaking ones. */
inline bool is_jvm_whitespace(std::uint32_t cp) {
    return (flags_of(cp) & kJvmWhitespace) != 0;
}

/** One decoded code point. Malformed input yields a single raw byte with length 1. */
struct Decoded {
    std::uint32_t code_point;
    unsigned length;
};

Decoded decode_utf8(const char* data, std::size_t size);

/** Offset of the code point that ends at `offset`, scanning UTF-8 continuation bytes back. */
std::size_t previous_offset(const char* data, std::size_t offset);

}  // namespace agora::core::unicode
