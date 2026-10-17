#pragma once

// Shared helpers for the stream-normalization port. Not installed: core-internal only.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "agora/core/json.hpp"
#include "agora/core/unicode.hpp"

namespace agora::core::stream_normalization::detail {

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/** ASCII case-insensitive prefix test (all pinned markers and probes are ASCII). */
inline bool starts_with_ci(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (ascii_lower(text[index]) != ascii_lower(prefix[index])) return false;
    }
    return true;
}

/** ASCII case-insensitive substring test (Kotlin String.contains(.., ignoreCase = true)). */
inline bool contains_ci(std::string_view text, std::string_view needle) {
    if (needle.empty()) return true;
    if (text.size() < needle.size()) return false;
    for (std::size_t index = 0; index + needle.size() <= text.size(); ++index) {
        if (starts_with_ci(text.substr(index), needle)) return true;
    }
    return false;
}

/** ASCII case-insensitive indexOf; npos when absent. */
inline std::size_t find_ci(std::string_view text, std::string_view needle, std::size_t from = 0) {
    if (needle.empty()) return from <= text.size() ? from : std::string_view::npos;
    if (from >= text.size() || text.size() - from < needle.size()) return std::string_view::npos;
    for (std::size_t index = from; index + needle.size() <= text.size(); ++index) {
        if (starts_with_ci(text.substr(index), needle)) return index;
    }
    return std::string_view::npos;
}

/** Byte-wise case-insensitive region equality with bounds (regionMatches). */
inline bool region_matches_ci(std::string_view text, std::size_t index, std::string_view other) {
    if (index + other.size() > text.size()) return false;
    for (std::size_t offset = 0; offset < other.size(); ++offset) {
        if (ascii_lower(text[index + offset]) != ascii_lower(other[offset])) return false;
    }
    return true;
}

/** java.util.regex `\s`: ASCII whitespace only, unlike Kotlin's Char.isWhitespace. */
inline bool is_java_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0B' || c == '\f' || c == '\r';
}

/** safeWireToolName = Regex("[A-Za-z0-9_-]{1,64}") — Kotlin matches() (full match). */
inline bool is_safe_wire_tool_name(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/** safeWireToolCallId = Regex("[A-Za-z0-9_-]{1,128}"). */
inline bool is_safe_wire_tool_call_id(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    for (char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/**
 * Kotlin String.trimStart(): leading characters removed while Char.isWhitespace.
 * json.hpp's predicate is the same set the pinned reference trim() uses.
 */
inline std::string trim_start(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size()) {
        const auto decoded = agora::core::unicode::decode_utf8(text.data() + start, text.size() - start);
        if (!agora::core::json::is_kotlin_whitespace_code_point(decoded.code_point)) break;
        start += decoded.length;
    }
    return std::string(text.substr(start));
}

/** Number of bytes in the UTF-8 sequence starting at `data`; clamped to `size`. */
inline std::size_t utf8_sequence_length(const char* data, std::size_t size) {
    if (size == 0) return 0;
    const unsigned char lead = static_cast<unsigned char>(data[0]);
    std::size_t length = 1;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
    }
    return length > size ? size : length;
}

}  // namespace agora::core::stream_normalization::detail
