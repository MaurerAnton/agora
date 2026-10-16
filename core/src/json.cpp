#include "agora/core/json.hpp"

#include <cstring>

namespace agora::core::json {
namespace {

// Token classes of the reference lexer (AbstractJsonLexer).
enum : unsigned char {
    kTokenOther = 0,
    kTokenString = 1,
    kTokenStringEscape = 2,
    kTokenWhitespace = 3,
    kTokenComma = 4,
    kTokenColon = 5,
    kTokenBeginObject = 6,
    kTokenEndObject = 7,
    kTokenBeginArray = 8,
    kTokenEndArray = 9,
    kTokenInvalid = 127,
};

unsigned char token_class(unsigned char byte) {
    switch (byte) {
        case 0x09:
        case 0x0a:
        case 0x0d:
        case 0x20:
            return kTokenWhitespace;
        case ',':
            return kTokenComma;
        case ':':
            return kTokenColon;
        case '{':
            return kTokenBeginObject;
        case '}':
            return kTokenEndObject;
        case '[':
            return kTokenBeginArray;
        case ']':
            return kTokenEndArray;
        case '"':
            return kTokenString;
        case '\\':
            return kTokenStringEscape;
        default:
            // 0x00-0x08, 0x0b, 0x0c, 0x0e-0x1f are TC_INVALID; everything from
            // 0x21 up (including 0x7e+, so all UTF-8 continuation bytes) is TC_OTHER.
            return byte < 0x20 ? kTokenInvalid : kTokenOther;
    }
}

int hex_digit(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

struct Parser {
    std::string_view source;
    std::size_t pos = 0;
    bool failed = false;
    std::size_t error_position = 0;
    std::string error;

    bool fail(std::string message) {
        if (!failed) {
            failed = true;
            error_position = pos;
            error = std::move(message);
        }
        return false;
    }

    bool at_end() const { return pos >= source.size(); }
    unsigned char peek() const { return static_cast<unsigned char>(source[pos]); }

    void skip_ws() {
        while (!at_end()) {
            unsigned char byte = peek();
            if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    /** Reads 4 hex digits at [at], without consuming. */
    bool peek_hex4(std::size_t at, std::uint32_t& unit) const {
        if (at + 4 > source.size()) return false;
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            int digit = hex_digit(static_cast<unsigned char>(source[at + i]));
            if (digit < 0) return false;
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        unit = value;
        return true;
    }

    bool parse_hex4(std::uint32_t& unit) {
        if (!peek_hex4(pos, unit)) return fail("Invalid unicode escape");
        pos += 4;
        return true;
    }

    bool parse_string_token(Value& out) {
        out.kind = Kind::String;
        ++pos;  // opening quote
        std::string& text = out.text;
        while (true) {
            if (at_end()) return fail("Unexpected end of input inside a string");
            unsigned char byte = peek();
            if (byte == '"') {
                ++pos;
                out.end = pos;
                return true;
            }
            if (byte == '\\') {
                ++pos;
                if (at_end()) return fail("Unexpected end of input inside a string escape");
                unsigned char escape = peek();
                switch (escape) {
                    case '"': text.push_back('"'); ++pos; break;
                    case '\\': text.push_back('\\'); ++pos; break;
                    case '/': text.push_back('/'); ++pos; break;
                    case 'b': text.push_back('\b'); ++pos; break;
                    case 'f': text.push_back('\f'); ++pos; break;
                    case 'n': text.push_back('\n'); ++pos; break;
                    case 'r': text.push_back('\r'); ++pos; break;
                    case 't': text.push_back('\t'); ++pos; break;
                    case 'u': {
                        ++pos;
                        std::uint32_t unit = 0;
                        if (!parse_hex4(unit)) return false;
                        if (unit >= 0xD800 && unit <= 0xDBFF) {
                            // The JVM string holds the code unit; its UTF-8 output
                            // pairs an adjacent low surrogate and renders an
                            // unpaired surrogate as '?'. Mirror that here so the
                            // decoded content already equals the observable text.
                            std::uint32_t low = 0;
                            if (pos < source.size() && source[pos] == '\\' &&
                                pos + 1 < source.size() && source[pos + 1] == 'u' &&
                                peek_hex4(pos + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                                pos += 6;
                                append_utf8(text, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                            } else {
                                text.push_back('?');
                            }
                        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
                            text.push_back('?');
                        } else {
                            append_utf8(text, unit);
                        }
                        break;
                    }
                    default:
                        return fail("Invalid escape sequence");
                }
                continue;
            }
            // Every other byte is content, including raw control bytes: the
            // reference lexer validates escapes only.
            text.push_back(static_cast<char>(byte));
            ++pos;
        }
    }

    bool parse_literal(Value& out) {
        out.kind = Kind::Literal;
        std::size_t start = pos;
        while (!at_end() && token_class(peek()) == kTokenOther) ++pos;
        out.text = std::string(source.substr(start, pos - start));
        out.end = pos;
        return true;
    }

    bool parse_object(Value& out) {
        out.kind = Kind::Object;
        ++pos;  // '{'
        skip_ws();
        if (at_end()) return fail("Unexpected end of input inside an object");
        if (peek() == '}') {
            ++pos;
            out.end = pos;
            return true;
        }
        while (true) {
            skip_ws();
            if (at_end()) return fail("Unexpected end of input inside an object");
            if (token_class(peek()) != kTokenString) {
                return fail("Expected a quoted object key");
            }
            Value key;
            if (!parse_string_token(key)) return false;
            skip_ws();
            if (at_end() || peek() != ':') return fail("Expected ':' after an object key");
            ++pos;
            Value value;
            if (!parse_value(value)) return false;
            out.members.emplace_back(Member{std::move(key.text), std::move(value)});
            skip_ws();
            if (at_end()) return fail("Unexpected end of input inside an object");
            if (peek() == ',') {
                ++pos;
                skip_ws();
                if (!at_end() && peek() == '}') return fail("Unexpected trailing comma");
                continue;
            }
            if (peek() == '}') {
                ++pos;
                out.end = pos;
                return true;
            }
            return fail("Expected end of the object or comma");
        }
    }

    bool parse_array(Value& out) {
        out.kind = Kind::Array;
        ++pos;  // '['
        skip_ws();
        if (at_end()) return fail("Unexpected end of input inside an array");
        if (peek() == ']') {
            ++pos;
            out.end = pos;
            return true;
        }
        while (true) {
            Value element;
            if (!parse_value(element)) return false;
            out.elements.push_back(std::move(element));
            skip_ws();
            if (at_end()) return fail("Unexpected end of input inside an array");
            if (peek() == ',') {
                ++pos;
                skip_ws();
                if (!at_end() && peek() == ']') return fail("Unexpected trailing comma");
                continue;
            }
            if (peek() == ']') {
                ++pos;
                out.end = pos;
                return true;
            }
            return fail("Expected end of the array or comma");
        }
    }

    bool parse_value(Value& out) {
        skip_ws();
        if (at_end()) return fail("Unexpected end of input");
        out.begin = pos;
        switch (token_class(peek())) {
            case kTokenString:
                return parse_string_token(out);
            case kTokenBeginObject:
                return parse_object(out);
            case kTokenBeginArray:
                return parse_array(out);
            case kTokenOther:
                return parse_literal(out);
            default:
                return fail("Unexpected symbol at a value position");
        }
    }
};

std::size_t decode_utf8(std::string_view text, std::size_t pos, std::uint32_t& code_point) {
    unsigned char first = static_cast<unsigned char>(text[pos]);
    if (first < 0x80) {
        code_point = first;
        return pos + 1;
    }
    auto continuation = [&](std::size_t at) -> int {
        if (at >= text.size()) return -1;
        unsigned char byte = static_cast<unsigned char>(text[at]);
        return (byte & 0xC0) == 0x80 ? (byte & 0x3F) : -1;
    };
    if (first >= 0xC2 && first <= 0xDF) {
        int second = continuation(pos + 1);
        if (second >= 0) {
            code_point = ((first & 0x1F) << 6) | static_cast<std::uint32_t>(second);
            return pos + 2;
        }
    } else if (first >= 0xE0 && first <= 0xEF) {
        int second = continuation(pos + 1);
        int third = continuation(pos + 2);
        if (second >= 0 && third >= 0) {
            code_point = ((first & 0x0F) << 12) | (static_cast<std::uint32_t>(second) << 6) |
                         static_cast<std::uint32_t>(third);
            return pos + 3;
        }
    } else if (first >= 0xF0 && first <= 0xF4) {
        int second = continuation(pos + 1);
        int third = continuation(pos + 2);
        int fourth = continuation(pos + 3);
        if (second >= 0 && third >= 0 && fourth >= 0) {
            code_point = ((first & 0x07) << 18) | (static_cast<std::uint32_t>(second) << 12) |
                         (static_cast<std::uint32_t>(third) << 6) | static_cast<std::uint32_t>(fourth);
            return pos + 4;
        }
    }
    // Invalid byte: decode as itself; it can never be whitespace.
    code_point = first;
    return pos + 1;
}

void append_kotlin_string(std::string& out, const Value& value) {
    switch (value.kind) {
        case Kind::String:
            out.push_back('"');
            escape_to(out, value.text);
            out.push_back('"');
            return;
        case Kind::Literal:
            out += value.text;
            return;
        case Kind::Array: {
            out.push_back('[');
            bool first = true;
            for (const Value& element : value.elements) {
                if (!first) out.push_back(',');
                first = false;
                append_kotlin_string(out, element);
            }
            out.push_back(']');
            return;
        }
        case Kind::Object: {
            out.push_back('{');
            bool first = true;
            for (std::size_t i = 0; i < value.members.size(); ++i) {
                const std::string& key = value.members[i].key;
                // LinkedHashMap semantics: a later duplicate updates the value
                // but keeps the first insertion position.
                bool has_earlier = false;
                for (std::size_t j = 0; j < i; ++j) {
                    if (value.members[j].key == key) {
                        has_earlier = true;
                        break;
                    }
                }
                if (has_earlier) continue;
                const Value* result = &value.members[i].value;
                for (std::size_t j = value.members.size(); j-- > i + 1;) {
                    if (value.members[j].key == key) {
                        result = &value.members[j].value;
                        break;
                    }
                }
                if (!first) out.push_back(',');
                first = false;
                out.push_back('"');
                escape_to(out, key);
                out.push_back('"');
                out.push_back(':');
                append_kotlin_string(out, *result);
            }
            out.push_back('}');
            return;
        }
    }
}

}  // namespace

ParseResult parse(std::string_view source) {
    Parser parser;
    parser.source = source;
    ParseResult result;
    Value value;
    if (!parser.parse_value(value)) {
        result.error_position = parser.error_position;
        result.error = std::move(parser.error);
        return result;
    }
    parser.skip_ws();
    if (!parser.at_end()) {
        result.error_position = parser.pos;
        result.error = "Expected EOF after parsing, but got content instead";
        return result;
    }
    result.value = std::move(value);
    return result;
}

void escape_to(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (char raw : text) {
        unsigned char byte = static_cast<unsigned char>(raw);
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\f': out += "\\f"; break;
            default:
                if (byte < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[(byte >> 4) & 0xF]);
                    out.push_back(kHex[byte & 0xF]);
                } else {
                    out.push_back(raw);
                }
        }
    }
}

std::string to_kotlin_string(const Value& value) {
    std::string out;
    append_kotlin_string(out, value);
    return out;
}

bool is_kotlin_whitespace_code_point(std::uint32_t code_point) {
    switch (code_point) {
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
        case 0x20:
        case 0xA0:
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000:
            return true;
        default:
            return code_point >= 0x2000 && code_point <= 0x200A;
    }
}

std::string trim(std::string_view text) {
    std::size_t start = 0;
    std::uint32_t code_point = 0;
    while (start < text.size()) {
        std::size_t next = decode_utf8(text, start, code_point);
        if (!is_kotlin_whitespace_code_point(code_point)) break;
        start = next;
    }
    std::size_t end = start;
    std::size_t scan = start;
    while (scan < text.size()) {
        std::size_t next = decode_utf8(text, scan, code_point);
        if (!is_kotlin_whitespace_code_point(code_point)) end = next;
        scan = next;
    }
    return std::string(text.substr(start, end - start));
}

bool is_blank(std::string_view text) {
    std::size_t pos = 0;
    std::uint32_t code_point = 0;
    while (pos < text.size()) {
        std::size_t next = decode_utf8(text, pos, code_point);
        if (!is_kotlin_whitespace_code_point(code_point)) return false;
        pos = next;
    }
    return true;
}

std::string take_utf16_units(std::string_view text, std::size_t units) {
    std::string out;
    std::size_t pos = 0;
    std::size_t used = 0;
    std::uint32_t code_point = 0;
    while (pos < text.size()) {
        std::size_t next = decode_utf8(text, pos, code_point);
        std::size_t width = code_point > 0xFFFF ? 2 : 1;
        if (used + width > units) {
            // A pair split exactly at the cut leaves one unpaired surrogate,
            // which the JVM's UTF-8 output renders as '?'.
            if (width == 2 && used + 1 == units) out.push_back('?');
            break;
        }
        out.append(text.substr(pos, next - pos));
        used += width;
        pos = next;
    }
    return out;
}

std::size_t utf16_length(std::string_view text) {
    std::size_t units = 0;
    std::size_t pos = 0;
    std::uint32_t code_point = 0;
    while (pos < text.size()) {
        pos = decode_utf8(text, pos, code_point);
        units += code_point > 0xFFFF ? 2 : 1;
    }
    return units;
}

bool is_other_token_byte(unsigned char byte) {
    return token_class(byte) == kTokenOther;
}

}  // namespace agora::core::json
