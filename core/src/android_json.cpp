#include "agora/core/android_json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace agora::core::android_json {

void read_to_end(Source& source) {
    char scratch[8192];
    while (source.read(scratch, sizeof(scratch)) >= 0) {
    }
}

std::string token_name(Token token) {
    switch (token) {
        case Token::BeginArray: return "BEGIN_ARRAY";
        case Token::EndArray: return "END_ARRAY";
        case Token::BeginObject: return "BEGIN_OBJECT";
        case Token::EndObject: return "END_OBJECT";
        case Token::Name: return "NAME";
        case Token::String: return "STRING";
        case Token::Number: return "NUMBER";
        case Token::Boolean: return "BOOLEAN";
        case Token::Null: return "NULL";
        case Token::EndDocument: return "END_DOCUMENT";
    }
    return "END_DOCUMENT";
}

std::string java_long_to_string(long long value) { return std::to_string(value); }

std::string java_double_to_string(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    const bool negative = std::signbit(value);
    if (value == 0.0) return negative ? "-0.0" : "0.0";

    // Shortest roundtrip digits (std::to_chars general), then Java's Double.toString layout:
    // scientific iff decimal exponent < -3 or >= 7, always with one fractional digit there.
    char scratch[48];
    const auto result = std::to_chars(scratch, scratch + sizeof(scratch), std::fabs(value));
    if (result.ec != std::errc()) return negative ? "-Infinity" : "Infinity";
    std::string text(scratch, result.ptr);

    std::string mantissa = text;
    int exponent = 0;
    if (const auto e = text.find_first_of("eE"); e != std::string::npos) {
        mantissa = text.substr(0, e);
        exponent = std::atoi(text.c_str() + e + 1);
    } else {
        // Decimal input: value = intpart.frac with intpart a single digit or "0".
        const auto dot = mantissa.find('.');
        const std::string int_part = mantissa.substr(0, dot);
        const std::string frac_part =
            dot == std::string::npos ? std::string() : mantissa.substr(dot + 1);
        if (int_part != "0") {
            exponent = static_cast<int>(int_part.size()) - 1;
            mantissa = int_part + frac_part;
        } else {
            std::size_t leading = 0;
            while (leading < frac_part.size() && frac_part[leading] == '0') ++leading;
            exponent = -static_cast<int>(leading) - 1;
            mantissa = frac_part.substr(leading);
        }
    }

    std::string digits;
    for (const char c : mantissa) {
        if (c != '.') digits.push_back(c);
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    if (digits.empty()) digits = "0";

    const std::string sign = negative ? "-" : "";
    if (exponent >= -3 && exponent < 7) {
        std::string out = sign;
        if (exponent >= 0) {
            if (static_cast<int>(digits.size()) <= exponent + 1) {
                out += digits;
                out.append(static_cast<std::size_t>(exponent + 1) - digits.size(), '0');
                out += ".0";
            } else {
                out += digits.substr(0, static_cast<std::size_t>(exponent) + 1);
                out += '.';
                out += digits.substr(static_cast<std::size_t>(exponent) + 1);
            }
        } else {
            out += "0.";
            out.append(static_cast<std::size_t>(-exponent - 1), '0');
            out += digits;
        }
        return out;
    }
    std::string out = sign;
    out.push_back(digits[0]);
    out += '.';
    if (digits.size() > 1) out += digits.substr(1);
    else out += "0";
    out += 'E';
    out += std::to_string(exponent);
    return out;
}

double java_parse_double(std::string_view text) {
    double value = 0.0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        throw Exception("NumberFormatException", "For input string: \"" + std::string(text) + "\"");
    }
    return value;
}

// -----------------------------------------------------------------------------------------------
// Reader
// -----------------------------------------------------------------------------------------------

namespace {
constexpr char kSentinel = '\0';
}  // namespace

Reader::Reader(Source& source)
    : in_(source), buffer_(kBufferSize + 1, '\0') {
    stack_.push_back(Scope::EmptyDocument);
}

Reader::Scope Reader::stack_top() const { return stack_.back(); }

void Reader::push(Scope kind) { stack_.push_back(kind); }

Reader::Scope Reader::pop() {
    const Scope top = stack_.back();
    stack_.pop_back();
    return top;
}

void Reader::replace_top(Scope kind) { stack_.back() = kind; }

Exception Reader::syntax_error(std::string_view message) {
    std::string text(message);
    text += " at line ";
    text += std::to_string(line_number());
    text += " column ";
    text += std::to_string(column_number());
    return Exception("MalformedJsonException", text);
}

bool Reader::decode_one(const char* data, std::size_t size, bool at_eof, std::string& out,
                        std::size_t& consumed) {
    if (size == 0) {
        consumed = 0;
        return true;
    }
    const auto lead = static_cast<unsigned char>(data[0]);

    auto emit_replacement = [&](std::size_t take) {
        out += "\xEF\xBF\xBD";  // U+FFFD
        consumed = take;
        return true;
    };

    if (lead < 0x80) {
        out.push_back(static_cast<char>(lead));
        consumed = 1;
        return true;
    }

    std::size_t expected = 0;
    unsigned char lo1 = 0x80, hi1 = 0xBF;
    unsigned char lo2 = 0x80, hi2 = 0xBF;
    unsigned char lo3 = 0x80, hi3 = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        expected = 2;
    } else if (lead == 0xE0) {
        expected = 3;
        lo1 = 0xA0;
    } else if (lead >= 0xE1 && lead <= 0xEC) {
        expected = 3;
    } else if (lead == 0xED) {
        expected = 3;
        hi2 = 0x9F;  // no UTF-16 surrogates
    } else if (lead >= 0xEE && lead <= 0xEF) {
        expected = 3;
    } else if (lead == 0xF0) {
        expected = 4;
        lo1 = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        expected = 4;
    } else if (lead == 0xF4) {
        expected = 4;
        hi1 = 0x8F;
    } else {
        return emit_replacement(1);  // 0x80..0xC1, 0xF5..0xFF
    }

    // Validate continuations with the range restrictions that reject overlongs and surrogates.
    if (size < 2) {
        if (at_eof) return emit_replacement(1);
        return false;
    }
    const unsigned char b1 = static_cast<unsigned char>(data[1]);
    if (b1 < lo1 || b1 > hi1) return emit_replacement(1);
    if (expected == 2) {
        out.push_back(static_cast<char>(lead));
        out.push_back(static_cast<char>(b1));
        consumed = 2;
        return true;
    }
    if (size < 3) {
        if (at_eof) return emit_replacement(2);
        return false;
    }
    const unsigned char b2 = static_cast<unsigned char>(data[2]);
    if (b2 < lo2 || b2 > hi2) {
        // UAX-19 maximal subpart: lead+b1 is the longest prefix that could still complete.
        out += "\xEF\xBF\xBD";
        consumed = 2;
        return true;
    }
    if (expected == 3) {
        out.push_back(static_cast<char>(lead));
        out.push_back(static_cast<char>(b1));
        out.push_back(static_cast<char>(b2));
        consumed = 3;
        return true;
    }
    if (size < 4) {
        if (at_eof) return emit_replacement(3);
        return false;
    }
    const unsigned char b3 = static_cast<unsigned char>(data[3]);
    if (b3 < lo3 || b3 > hi3) {
        out += "\xEF\xBF\xBD";
        consumed = 3;
        return true;
    }
    out.push_back(static_cast<char>(lead));
    out.push_back(static_cast<char>(b1));
    out.push_back(static_cast<char>(b2));
    out.push_back(static_cast<char>(b3));
    consumed = 4;
    return true;
}

bool Reader::fill_buffer(std::size_t minimum) {
    // AOSP: before clobbering, account for the consumed characters in line/column space.
    std::size_t i = 0;
    while (i < pos_) {
        if (buffer_[i] == '\n') {
            ++buffer_start_line_;
            buffer_start_column_ = 1;
            ++i;
            continue;
        }
        const auto byte = static_cast<unsigned char>(buffer_[i]);
        if (byte < 0x80) {
            ++buffer_start_column_;
            ++i;
        } else if ((byte & 0xE0) == 0xC0) {
            ++buffer_start_column_;
            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            ++buffer_start_column_;
            i += 3;
        } else {
            buffer_start_column_ += 2;  // astral = a surrogate pair in Java's buffer
            i += 4;
        }
    }
    if (limit_ != pos_) {
        limit_ -= pos_;
        std::memmove(buffer_.data(), buffer_.data() + pos_, limit_);
    } else {
        limit_ = 0;
    }
    pos_ = 0;

    while (true) {
        // Decode whatever is pending (carry_) up to the buffer limit, codepoint by codepoint,
        // so the buffer never splits a character — Java's InputStreamReader never does either.
        while (!carry_.empty()) {
            if (buffer_start_line_ == 1 && buffer_start_column_ == 1 && limit_ == 0 &&
                carry_.size() >= 3 &&
                static_cast<unsigned char>(carry_[0]) == 0xEF &&
                static_cast<unsigned char>(carry_[1]) == 0xBB &&
                static_cast<unsigned char>(carry_[2]) == 0xBF) {
                // AOSP consumes a leading BOM as one character and gives its column back.
                carry_.erase(0, 3);
                buffer_start_column_ -= 1;
                continue;
            }
            std::string decoded;
            std::size_t consumed = 0;
            const bool complete = decode_one(carry_.data(), carry_.size(), source_eof_, decoded,
                                             consumed);
            if (!complete) break;  // need more input to finish the sequence
            if (consumed == 0) break;
            carry_.erase(0, consumed);
            if (limit_ + decoded.size() <= kBufferSize) {
                std::copy(decoded.begin(), decoded.end(), buffer_.begin() +
                          static_cast<std::ptrdiff_t>(limit_));
                limit_ += decoded.size();
            } else {
                // The decoded codepoint cannot fit: put it back and stop (AOSP would fill up to
                // its buffer end and let the caller see limit_ < minimum).
                carry_.insert(0, decoded);
                return limit_ >= minimum;
            }
            if (limit_ >= kBufferSize) return limit_ >= minimum;
        }
        if (limit_ >= minimum) return true;
        if (source_eof_) return limit_ >= minimum;
        // Pull more raw bytes.
        char scratch[1024];
        const long long total = in_.read(scratch, sizeof(scratch));
        if (total == -1) {
            source_eof_ = true;
            continue;
        }
        carry_.append(scratch, static_cast<std::size_t>(total));
        if (limit_ >= kBufferSize) return limit_ >= minimum;
    }
}

std::size_t Reader::line_number() const {
    std::size_t result = buffer_start_line_;
    for (std::size_t i = 0; i < pos_; ++i) {
        if (buffer_[i] == '\n') ++result;
    }
    return result;
}

std::size_t Reader::column_number() const {
    // AOSP: start from the buffer's base column and walk the consumed characters, resetting
    // at every newline; astral codepoints count as the two units Java's char buffer uses.
    std::size_t result = buffer_start_column_;
    std::size_t i = 0;
    while (i < pos_) {
        if (buffer_[i] == '\n') {
            result = 1;
            ++i;
            continue;
        }
        const auto byte = static_cast<unsigned char>(buffer_[i]);
        if (byte < 0x80) {
            ++result;
            ++i;
        } else if ((byte & 0xE0) == 0xC0) {
            ++result;
            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            ++result;
            i += 3;
        } else {
            result += 2;
            i += 4;
        }
    }
    return result;
}

// ---- token family ------------------------------------------------------------------------------

namespace {
constexpr char kEnd = '\0';

/** Encodes one code point as UTF-8 (the reference encoder's output for valid pairs). */
std::string utf8_encode(std::uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

}  // namespace

void Reader::begin_array() {
    peek();
    if (token_ != Token::BeginArray) {
        throw Exception("IllegalStateException",
                        "Expected BEGIN_ARRAY but was " + token_name(peek()));
    }
    token_set_ = false;
    has_value_ = false;
    name_.clear();
    value_.clear();
}

void Reader::end_array() {
    peek();
    if (token_ != Token::EndArray) {
        throw Exception("IllegalStateException",
                        "Expected END_ARRAY but was " + token_name(peek()));
    }
    token_set_ = false;
    has_value_ = false;
    name_.clear();
    value_.clear();
}

void Reader::begin_object() {
    peek();
    if (token_ != Token::BeginObject) {
        throw Exception("IllegalStateException",
                        "Expected BEGIN_OBJECT but was " + token_name(peek()));
    }
    token_set_ = false;
    has_value_ = false;
    name_.clear();
    value_.clear();
}

void Reader::end_object() {
    peek();
    if (token_ != Token::EndObject) {
        throw Exception("IllegalStateException",
                        "Expected END_OBJECT but was " + token_name(peek()));
    }
    token_set_ = false;
    has_value_ = false;
    name_.clear();
    value_.clear();
}

bool Reader::has_next() {
    peek();
    return token_ != Token::EndObject && token_ != Token::EndArray;
}

Token Reader::peek() {
    if (token_set_) return token_;

    switch (stack_top()) {
        case Scope::EmptyDocument: {
            replace_top(Scope::NonemptyDocument);
            const Token first = next_value();
            if (first != Token::BeginArray && first != Token::BeginObject) {
                throw Exception("IOException",
                                "Expected JSON document to start with '[' or '{' but was " +
                                    token_name(first));
            }
            token_ = first;
            token_set_ = true;
            return token_;
        }
        case Scope::EmptyArray:
            token_ = next_in_array(true);
            token_set_ = true;
            return token_;
        case Scope::NonemptyArray:
            token_ = next_in_array(false);
            token_set_ = true;
            return token_;
        case Scope::EmptyObject:
            token_ = next_in_object(true);
            token_set_ = true;
            return token_;
        case Scope::DanglingName:
            token_ = object_value();
            token_set_ = true;
            return token_;
        case Scope::NonemptyObject:
            token_ = next_in_object(false);
            token_set_ = true;
            return token_;
        case Scope::NonemptyDocument:
            try {
                const Token next = next_value();
                (void)next;
                throw syntax_error("Expected EOF");
            } catch (const Exception& error) {
                if (error.java_class() == "EOFException") {
                    token_ = Token::EndDocument;
                    token_set_ = true;
                    return token_;
                }
                throw;
            }
        case Scope::Closed:
            throw Exception("IllegalStateException", "JsonReader is closed");
    }
    throw Exception("IllegalStateException", "JsonReader is closed");
}

std::string Reader::next_name() {
    peek();
    if (token_ != Token::Name) {
        throw Exception("IllegalStateException", "Expected a name but was " + token_name(peek()));
    }
    std::string result = name_;
    token_set_ = false;
    has_value_ = false;
    name_.clear();
    value_.clear();
    return result;
}

std::string Reader::next_string() {
    peek();
    if (token_ != Token::String && token_ != Token::Number) {
        throw Exception("IllegalStateException",
                        "Expected a string but was " + token_name(peek()));
    }
    std::string result = value_;
    token_set_ = false;
    has_value_ = false;
    name_.clear();
    value_.clear();
    return result;
}

bool Reader::next_boolean() {
    peek();
    if (token_ != Token::Boolean) {
        throw Exception("IllegalStateException", "Expected a boolean but was " + token_name(token_));
    }
    const bool result = value_ == "true";
    token_set_ = false;
    has_value_ = false;
    name_.clear();
    value_.clear();
    return result;
}

void Reader::next_null() {
    peek();
    if (token_ != Token::Null) {
        throw Exception("IllegalStateException", "Expected null but was " + token_name(token_));
    }
    token_set_ = false;
    has_value_ = false;
    name_.clear();
    value_.clear();
}

void Reader::skip_value() {
    if (!has_next() || peek() == Token::EndDocument) {
        throw Exception("IllegalStateException", "No element left to skip");
    }
    int count = 0;
    do {
        const Token token = peek();
        token_set_ = false;
        has_value_ = false;
        name_.clear();
        value_.clear();
        if (token == Token::BeginArray || token == Token::BeginObject) {
            ++count;
        } else if (token == Token::EndArray || token == Token::EndObject) {
            --count;
        }
    } while (count != 0);
}

// ---- state machine -------------------------------------------------------------------------------

int Reader::next_non_whitespace() {
    while (pos_ < limit_ || fill_buffer(1)) {
        const char c = buffer_[pos_++];
        switch (c) {
            case '\t':
            case ' ':
            case '\n':
            case '\r':
                continue;
            case '/':
                // AOSP: a '/' at the very end of input is returned as a value character; any
                // other slash enters the (lenient-only) comment path, which strict mode rejects.
                if (pos_ == limit_ && !fill_buffer(1)) return c;
                check_lenient();
                return c;
            case '#':
                check_lenient();
                return c;
            default:
                return c;
        }
    }
    throw Exception("EOFException", "End of input");
}

void Reader::check_lenient() {
    // This port is always strict (the app never enables leniency).
    throw syntax_error("Use JsonReader.setLenient(true) to accept malformed JSON");
}

Token Reader::next_in_array(bool first_element) {
    if (first_element) {
        replace_top(Scope::NonemptyArray);
    } else {
        switch (next_non_whitespace()) {
            case ']':
                pop();
                return Token::EndArray;
            case ';':
                check_lenient();  // strict: throws; the ',' fall-through is lenient-only
                return Token::EndArray;  // unreachable
            case ',':
                break;
            default:
                throw syntax_error("Unterminated array");
        }
    }

    switch (next_non_whitespace()) {
        case ']':
            if (first_element) {
                pop();
                return Token::EndArray;
            }
            [[fallthrough]];
        case ';':
        case ',':
            // A zero-length element is a lenient-only "null".
            check_lenient();
            pos_--;
            value_ = "null";
            has_value_ = true;
            return Token::Null;
        default:
            pos_--;
            return next_value();
    }
}

Token Reader::next_in_object(bool first_element) {
    if (first_element) {
        switch (next_non_whitespace()) {
            case '}':
                pop();
                return Token::EndObject;
            default:
                pos_--;
        }
    } else {
        switch (next_non_whitespace()) {
            case '}':
                pop();
                return Token::EndObject;
            case ';':
            case ',':
                break;
            default:
                throw syntax_error("Unterminated object");
        }
    }

    const int quote = next_non_whitespace();
    switch (quote) {
        case '\'':
            check_lenient();
            return Token::Name;  // unreachable
        case '"':
            name_ = read_string('"');
            break;
        default:
            // Unquoted name: AOSP rejects in strict mode BEFORE backing up, so the reported
            // position is the character after the name's first letter.
            check_lenient();
            return Token::Name;  // unreachable
    }

    replace_top(Scope::DanglingName);
    return Token::Name;
}

Token Reader::object_value() {
    switch (next_non_whitespace()) {
        case ':':
            break;
        case '=':
            check_lenient();
            return Token::Null;  // unreachable in strict mode
        default:
            throw syntax_error("Expected ':'");
    }

    replace_top(Scope::NonemptyObject);
    return next_value();
}

Token Reader::next_value() {
    const int c = next_non_whitespace();
    switch (c) {
        case '{':
            push(Scope::EmptyObject);
            return Token::BeginObject;
        case '[':
            push(Scope::EmptyArray);
            return Token::BeginArray;
        case '\'':
            check_lenient();
            return Token::String;  // unreachable
        case '"':
            value_ = read_string('"');
            has_value_ = true;
            return Token::String;
        default:
            pos_--;
            return read_literal();
    }
}

std::string Reader::read_string(char quote) {
    std::string out;
    bool pending_high = false;
    auto flush_high = [&] {
        if (pending_high) {
            out += '?';  // a lone surrogate escapes as '?' through the JVM's UTF-8 encoder
            pending_high = false;
        }
    };
    std::size_t start = pos_;

    while (true) {
        while (pos_ < limit_) {
            const char c = buffer_[pos_++];
            if (c == quote) {
                flush_high();
                out.append(buffer_.data() + start, pos_ - start - 1);
                return out;
            }
            if (c != '\\') continue;

            flush_high();
            out.append(buffer_.data() + start, pos_ - start - 1);
            start = pos_;

            // read_escape_character, inlined with UTF-8 emission (see the header's note).
            if (pos_ == limit_ && !fill_buffer(1)) {
                throw syntax_error("Unterminated escape sequence");
            }
            const char escaped = buffer_[pos_++];
            if (escaped == 'u') {
                if (pos_ + 4 > limit_ && !fill_buffer(4)) {
                    throw syntax_error("Unterminated escape sequence");
                }
                std::string hex(buffer_.data() + pos_, 4);
                pos_ += 4;
                std::uint32_t cp = 0;
                for (const char digit : hex) {
                    int value = -1;
                    if (digit >= '0' && digit <= '9') value = digit - '0';
                    else if (digit >= 'a' && digit <= 'f') value = digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') value = digit - 'A' + 10;
                    if (value < 0) {
                        throw Exception("NumberFormatException",
                                        "Character " + std::string(1, digit) +
                                            " is neither a decimal digit nor a Java identifier part");
                    }
                    cp = cp * 16 + static_cast<std::uint32_t>(value);
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    pending_high = true;
                    high_surrogate_ = cp;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    if (pending_high) {
                        const std::uint32_t full =
                            0x10000 + ((high_surrogate_ - 0xD800u) << 10) + (cp - 0xDC00u);
                        out += utf8_encode(full);
                        pending_high = false;
                    } else {
                        out += '?';
                    }
                } else {
                    out += utf8_encode(cp);
                }
                start = pos_;
                continue;
            }
            switch (escaped) {
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 'f': out += '\f'; break;
                default: {
                    // AOSP returns the escaped character itself; for multi-byte characters copy
                    // the whole codepoint (a valid surrogate pair recombines to the same bytes).
                    pos_--;  // re-read the codepoint starting at pos_
                    std::size_t consumed = 0;
                    std::string decoded;
                    if (decode_one(buffer_.data() + pos_, limit_ - pos_, true, decoded, consumed)) {
                        if (consumed == 0) consumed = 1;
                        out += decoded;
                        pos_ += consumed;
                    } else {
                        out.push_back(escaped);
                    }
                    break;
                }
            }
            start = pos_;
        }

        flush_high();
        out.append(buffer_.data() + start, pos_ - start);
        if (!fill_buffer(1)) break;
        start = pos_;
    }
    throw syntax_error("Unterminated string");
}

std::string Reader::next_literal(bool /*assign_offsets_only*/) {
    // Scans up to the delimiter; a literal longer than the buffer spills into a builder, and a
    // literal that reaches the end of input ends there (AOSP's buffer[limit] = '\0' sentinel is
    // implicit: decode_number walks value_ and treats index == size as the boundary).
    std::string builder;
    bool spilled = false;
    std::size_t i = 0;
    bool found_delimiter = false;

    while (!found_delimiter) {
        for (; pos_ + i < limit_; ++i) {
            const char c = buffer_[pos_ + i];
            bool delimiter = false;
            switch (c) {
                case '/':
                case '\\':
                case ';':
                case '#':
                case '=':
                    check_lenient();  // strict mode: throws here (lenient would break the scan)
                    delimiter = true;
                    break;
                case '{':
                case '}':
                case '[':
                case ']':
                case ':':
                case ',':
                case ' ':
                case '\t':
                case '\f':
                case '\r':
                case '\n':
                    delimiter = true;
                    break;
                default:
                    break;
            }
            if (delimiter) {
                found_delimiter = true;
                break;
            }
        }
        if (found_delimiter) break;

        if (i < kBufferSize) {
            if (fill_buffer(i + 1)) continue;
            break;  // end of input: the literal ends at the buffer end
        }

        // Too long for the buffer: keep appending from the buffer directly.
        builder.append(buffer_.data() + pos_, i);
        pos_ += i;
        i = 0;
        spilled = true;
        if (!fill_buffer(1)) break;
    }

    std::string result;
    if (!spilled) {
        result.assign(buffer_.data() + pos_, i);
    } else {
        builder.append(buffer_.data() + pos_, i);
        result = builder;
    }
    pos_ += i;
    value_ = result;
    has_value_ = true;
    return result;
}

Token Reader::read_literal() {
    next_literal(true);
    if (value_.empty()) throw syntax_error("Expected literal value");
    const Token token = decode_literal();
    if (token == Token::String) check_lenient();
    return token;
}

Token Reader::decode_literal() {
    auto equals_ignore_case = [](const std::string& text, std::string_view keyword) {
        if (text.size() != keyword.size()) return false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            char a = text[i];
            char b = keyword[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    };
    if (equals_ignore_case(value_, "null")) {
        value_ = "null";
        has_value_ = true;
        return Token::Null;
    }
    if (equals_ignore_case(value_, "true")) {
        value_ = "true";
        has_value_ = true;
        return Token::Boolean;
    }
    if (equals_ignore_case(value_, "false")) {
        value_ = "false";
        has_value_ = true;
        return Token::Boolean;
    }
    return decode_number(value_);
}

Token Reader::decode_number(const std::string& literal) {
    // AOSP decodeNumber over the literal text, with index == size standing in for the '\0'
    // sentinel it reads one past the token.
    std::size_t i = 0;
    char c = literal.empty() ? kEnd : literal[0];
    if (c == '-') {
        c = (++i < literal.size()) ? literal[i] : kEnd;
    }
    if (c == '0') {
        c = (++i < literal.size()) ? literal[i] : kEnd;
    } else if (c >= '1' && c <= '9') {
        do {
            c = (++i < literal.size()) ? literal[i] : kEnd;
        } while (c >= '0' && c <= '9');
    } else {
        return Token::String;
    }

    if (c == '.') {
        c = (++i < literal.size()) ? literal[i] : kEnd;
        while (c >= '0' && c <= '9') {
            c = (++i < literal.size()) ? literal[i] : kEnd;
        }
    }

    if (c == 'e' || c == 'E') {
        c = (++i < literal.size()) ? literal[i] : kEnd;
        if (c == '+' || c == '-') {
            c = (++i < literal.size()) ? literal[i] : kEnd;
        }
        if (c >= '0' && c <= '9') {
            do {
                c = (++i < literal.size()) ? literal[i] : kEnd;
            } while (c >= '0' && c <= '9');
        } else {
            return Token::String;
        }
    }

    if (i == literal.size()) return Token::Number;
    return Token::String;
}

// -----------------------------------------------------------------------------------------------
// Writer
// -----------------------------------------------------------------------------------------------

Writer::Scope Writer::peek() const { return stack_.back(); }

void Writer::replace_top(Scope scope) { stack_.back() = scope; }

void Writer::open(Scope empty, std::string_view bracket) {
    before_value(true);
    stack_.push_back(empty);
    out_ += bracket;
}

void Writer::close_scope(Scope empty, Scope nonempty, std::string_view bracket) {
    const Scope context = peek();
    if (context != nonempty && context != empty) {
        throw Exception("IllegalStateException", "Nesting problem: JsonWriter");
    }
    stack_.pop_back();
    out_ += bracket;
}

void Writer::begin_array() { open(Scope::EmptyArray, "["); }

void Writer::end_array() { close_scope(Scope::EmptyArray, Scope::NonemptyArray, "]"); }

void Writer::begin_object() { open(Scope::EmptyObject, "{"); }

void Writer::end_object() { close_scope(Scope::EmptyObject, Scope::NonemptyObject, "}"); }

void Writer::name(std::string_view name) {
    before_name();
    write_string(name);
}

void Writer::value_null() {
    before_value(false);
    out_ += "null";
}

void Writer::value_bool(bool value) {
    before_value(false);
    out_ += value ? "true" : "false";
}

void Writer::value_long(long long value) {
    before_value(false);
    out_ += java_long_to_string(value);
}

void Writer::value_double(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        throw Exception("IllegalArgumentException",
                        "Numeric values must be finite, but was " + java_double_to_string(value));
    }
    before_value(false);
    out_ += java_double_to_string(value);
}

void Writer::value_string(std::string_view value) {
    before_value(false);
    write_string(value);
}

void Writer::close() {
    if (peek() != Scope::NonemptyDocument) {
        throw Exception("IOException", "Incomplete document");
    }
}

void Writer::write_string(std::string_view value) {
    out_ += '"';
    std::size_t i = 0;
    const std::size_t size = value.size();
    while (i < size) {
        // U+2028 (E2 80 A8) and U+2029 (E2 80 A9) are escaped for JavaScript's benefit.
        if (i + 2 < size && static_cast<unsigned char>(value[i]) == 0xE2 &&
            static_cast<unsigned char>(value[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(value[i + 2]) == 0xA8 ||
             static_cast<unsigned char>(value[i + 2]) == 0xA9)) {
            const unsigned cp = static_cast<unsigned char>(value[i + 2]) == 0xA8 ? 0x2028 : 0x2029;
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", cp);
            out_ += escape;
            i += 3;
            continue;
        }
        const char raw = value[i];
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"':
            case '\\':
                out_ += '\\';
                out_ += raw;
                break;
            case '\t': out_ += "\\t"; break;
            case '\b': out_ += "\\b"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\f': out_ += "\\f"; break;
            default:
                if (byte <= 0x1F) {
                    char escape[8];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
                    out_ += escape;
                } else {
                    out_ += raw;
                }
                break;
        }
        ++i;
    }
    out_ += '"';
}

void Writer::before_name() {
    const Scope context = peek();
    if (context == Scope::NonemptyObject) {
        out_ += ',';
    } else if (context != Scope::EmptyObject) {
        throw Exception("IllegalStateException", "Nesting problem: JsonWriter");
    }
    replace_top(Scope::DanglingName);
}

void Writer::before_value(bool root) {
    switch (peek()) {
        case Scope::EmptyDocument:
            if (!root) {
                throw Exception("IllegalStateException",
                                "JSON must start with an array or an object.");
            }
            replace_top(Scope::NonemptyDocument);
            break;
        case Scope::EmptyArray:
            replace_top(Scope::NonemptyArray);
            break;
        case Scope::NonemptyArray:
            out_ += ',';
            break;
        case Scope::DanglingName:
            out_ += ":";
            replace_top(Scope::NonemptyObject);
            break;
        case Scope::NonemptyDocument:
            throw Exception("IllegalStateException", "JSON must have only one top-level value.");
        default:
            throw Exception("IllegalStateException", "Nesting problem: JsonWriter");
    }
}

}  // namespace agora::core::android_json
