#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The JDK's REPLACE-mode UTF-8 decoder, shared by the java.net.URI escaped-component port and
// the shell file helpers. A malformed maximal subpart becomes one U+FFFD and the remaining
// octets are re-examined, exactly like `new String(bytes, StandardCharsets.UTF_8)`.
//
// This body is lifted from java_uri.cpp's decode_utf8_replace; keeping one copy means the
// URI decoder and `readBoundedShellOutput` cannot drift apart.
namespace agora::core::java_utf8 {

inline void decode_replace(std::string_view bytes, std::u16string& out) {
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        if (b0 < 0x80) {
            out.push_back(static_cast<char16_t>(b0));
            ++i;
            continue;
        }
        std::size_t need = 0;
        unsigned char second_lo = 0x80;
        unsigned char second_hi = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            need = 2;
        } else if (b0 == 0xE0) {
            need = 3;
            second_lo = 0xA0;
        } else if (b0 >= 0xE1 && b0 <= 0xEC) {
            need = 3;
        } else if (b0 == 0xED) {
            need = 3;
            second_hi = 0x9F;
        } else if (b0 >= 0xEE && b0 <= 0xEF) {
            need = 3;
        } else if (b0 == 0xF0) {
            need = 4;
            second_lo = 0x90;
        } else if (b0 >= 0xF1 && b0 <= 0xF3) {
            need = 4;
        } else if (b0 == 0xF4) {
            need = 4;
            second_hi = 0x8F;
        } else {
            // 80..C1 and F5..FF are never valid leads
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        std::size_t consumed = 1;
        bool complete = false;
        const auto next = [&](std::size_t offset) {
            return static_cast<unsigned char>(bytes[i + offset]);
        };
        if (i + 1 < n && next(1) >= second_lo && next(1) <= second_hi) {
            consumed = 2;
            if (need == 2) {
                complete = true;
            } else if (i + 2 < n && next(2) >= 0x80 && next(2) <= 0xBF) {
                consumed = 3;
                if (need == 3) {
                    complete = true;
                } else if (i + 3 < n && next(3) >= 0x80 && next(3) <= 0xBF) {
                    consumed = 4;
                    complete = true;
                }
            }
        }
        if (complete) {
            std::uint32_t code_point = 0;
            if (need == 2) {
                code_point = ((static_cast<std::uint32_t>(b0) & 0x1F) << 6) |
                             (next(1) & 0x3F);
            } else if (need == 3) {
                code_point = ((static_cast<std::uint32_t>(b0) & 0x0F) << 12) |
                             ((next(1) & 0x3F) << 6) | (next(2) & 0x3F);
            } else {
                code_point = ((static_cast<std::uint32_t>(b0) & 0x07) << 18) |
                             ((next(1) & 0x3F) << 12) | ((next(2) & 0x3F) << 6) |
                             (next(3) & 0x3F);
            }
            if (code_point <= 0xFFFF) {
                out.push_back(static_cast<char16_t>(code_point));
            } else {
                code_point -= 0x10000;
                out.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
                out.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FF)));
            }
            i += need;
        } else {
            out.push_back(0xFFFD);
            i += consumed;
        }
    }
}

}  // namespace agora::core::java_utf8
