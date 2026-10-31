#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "agora/server/submission_messages.hpp"

namespace agora::server {

/**
 * The escaping `json.dumps(..., ensure_ascii=False)` performs: only the RFC escapes, the
 * control bytes below 0x20 as lowercase \u00xx, everything else (including 0x7f and all
 * UTF-8 multibyte sequences) passes through raw.
 */
inline std::string py_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(byte >> 4) & 0xF];
                    out += kHex[byte & 0xF];
                } else {
                    out += raw;
                }
        }
    }
    return out;
}

/** Quoted string with py_escape applied. */
inline std::string py_string(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    out += py_escape(text);
    out += '"';
    return out;
}

/** Python `len(str)`: number of UTF-8 code points. */
inline std::size_t code_point_length(const std::string& text) {
    std::size_t count = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

/** Python `text[:limit]` over a str: never splits a code point. */
inline std::string take_code_points(std::string_view text, std::size_t limit) {
    std::size_t taken = 0;
    std::size_t index = 0;
    while (index < text.size() && taken < limit) {
        const auto byte = static_cast<unsigned char>(text[index]);
        const std::size_t width = byte < 0x80 ? 1 : (byte >> 5) == 0x6   ? 2
                                                : (byte >> 4) == 0xE      ? 3
                                                : (byte >> 3) == 0x1C     ? 4
                                                                          : 1;
        const std::size_t remaining = text.size() - index;
        const std::size_t take = width <= remaining ? width : remaining;
        index += take;
        ++taken;
    }
    return std::string(text.substr(0, index));
}

/** Strict UTF-8 validation matching Python's bytes.decode("utf-8"). */
inline bool valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto byte = static_cast<unsigned char>(text[index]);
        std::size_t extra;
        std::uint32_t code_point;
        if (byte < 0x80) {
            code_point = byte;
            extra = 0;
        } else if ((byte & 0xE0) == 0xC0) {
            code_point = byte & 0x1F;
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            code_point = byte & 0x0F;
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            code_point = byte & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (extra > 0 && index + extra >= text.size()) {
            return false;  // truncated sequence
        }
        for (std::size_t follow = 1; follow <= extra; ++follow) {
            const auto next = static_cast<unsigned char>(text[index + follow]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (next & 0x3F);
        }
        if (extra == 1 && code_point < 0x80) {
            return false;   // overlong
        }
        if (extra == 2 && code_point < 0x800) {
            return false;   // overlong
        }
        if (extra == 3 && code_point < 0x10000) {
            return false;   // overlong
        }
        if (code_point > 0x10FFFF) {
            return false;
        }
        if (code_point >= 0xD800 && code_point <= 0xDFFF) {
            return false;   // UTF-8 encoded surrogate, Python rejects it
        }
        index += extra + 1;
    }
    return true;
}

/** True for a literal that is a JSON number (no keywords: true/false/null are literals too). */
inline bool is_json_number(std::string_view text) {
    std::size_t index = 0;
    if (index < text.size() && (text[index] == '-' || text[index] == '+')) {
        ++index;
    }
    std::size_t digits = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        ++index;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    if (index < text.size() && text[index] == '.') {
        ++index;
        digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '-' || text[index] == '+')) {
            ++index;
        }
        digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
    }
    return index == text.size();
}

/** True for an integer literal python would accept into `isinstance(x, int)`. */
inline bool is_json_integer(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    std::size_t index = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (index >= text.size()) {
        return false;
    }
    for (; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    return true;
}

/** The message object with python's default separators (", " and ": "). */
inline std::string submission_message_json(const SubmissionMessage& message) {
    std::string out = "{\"id\": ";
    out += py_string(message.id);
    out += ", \"title\": ";
    out += py_string(message.title);
    out += ", \"body\": ";
    out += py_string(message.body);
    if (message.button_text.has_value()) {
        out += ", \"buttonText\": ";
        out += py_string(*message.button_text);
    }
    out += "}";
    return out;
}

}  // namespace agora::server
