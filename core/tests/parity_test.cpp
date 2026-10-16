#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"

#include "agora/core/bpe_vocabulary.hpp"
#include "agora/core/token_count.hpp"

#ifndef AGORA_FIXTURE_VOCABULARY
#error "AGORA_FIXTURE_VOCABULARY must point at app/src/main/assets/tokenizers/o200k_base.tiktoken"
#endif
#ifndef AGORA_FIXTURE_REFERENCE
#error "AGORA_FIXTURE_REFERENCE must point at app/src/test/resources/tokenizers/o200k_ref.json"
#endif

namespace {

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(std::string("cannot open ") + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
        code_point = 0xFFFD;
    }
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

class JsonCursor;

std::string skip_value(JsonCursor& cursor);

class JsonCursor {
public:
    explicit JsonCursor(std::string_view text) : text_(text) {}

    char peek() {
        skip_ws();
        return at_ < text_.size() ? text_[at_] : '\0';
    }

    void expect(char symbol) {
        const char found = peek();
        if (found != symbol) {
            throw std::runtime_error(
                std::string("json: expected '") + symbol + "' at " + std::to_string(at_) +
                " but found '" + found + "'");
        }
        at_++;
    }

    bool try_take(char symbol) {
        if (peek() != symbol) return false;
        at_++;
        return true;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (at_ >= text_.size()) throw std::runtime_error("json: unterminated string");
            const char symbol = text_[at_++];
            if (symbol == '"') break;
            if (symbol != '\\') {
                out.push_back(symbol);
                continue;
            }
            if (at_ >= text_.size()) throw std::runtime_error("json: unterminated escape");
            const char escape = text_[at_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code_point = read_hex4();
                    if (code_point >= 0xD800 && code_point <= 0xDBFF &&
                        at_ + 1 < text_.size() && text_[at_] == '\\' && text_[at_ + 1] == 'u') {
                        at_ += 2;
                        const std::uint32_t low = read_hex4();
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            append_utf8(out, 0xFFFD);
                        }
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default:
                    throw std::runtime_error("json: unknown escape");
            }
        }
        return out;
    }

    std::int64_t parse_int() {
        skip_ws();
        bool negative = false;
        if (at_ < text_.size() && text_[at_] == '-') {
            negative = true;
            at_++;
        }
        if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9') {
            throw std::runtime_error("json: expected integer");
        }
        std::int64_t value = 0;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            value = value * 10 + (text_[at_] - '0');
            at_++;
        }
        return negative ? -value : value;
    }

private:
    void skip_ws() {
        while (at_ < text_.size()) {
            const char symbol = text_[at_];
            if (symbol != ' ' && symbol != '\n' && symbol != '\r' && symbol != '\t') break;
            at_++;
        }
    }

    std::uint32_t read_hex4() {
        if (at_ + 4 > text_.size()) throw std::runtime_error("json: short \\u escape");
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char symbol = text_[at_++];
            value <<= 4;
            if (symbol >= '0' && symbol <= '9') value |= symbol - '0';
            else if (symbol >= 'a' && symbol <= 'f') value |= symbol - 'a' + 10;
            else if (symbol >= 'A' && symbol <= 'F') value |= symbol - 'A' + 10;
            else throw std::runtime_error("json: bad hex digit");
        }
        return value;
    }

    std::string_view text_;
    std::size_t at_ = 0;

    friend std::string skip_value(JsonCursor& cursor);
};

std::string skip_value(JsonCursor& cursor) {
    const char first = cursor.peek();
    if (first == '"') return cursor.parse_string();
    if (first == '-' || (first >= '0' && first <= '9')) {
        cursor.parse_int();
        return {};
    }
    if (first == '{') {
        cursor.expect('{');
        if (cursor.try_take('}')) return {};
        while (true) {
            cursor.parse_string();
            cursor.expect(':');
            skip_value(cursor);
            if (cursor.try_take(',')) continue;
            cursor.expect('}');
            return {};
        }
    }
    if (first == '[') {
        cursor.expect('[');
        if (cursor.try_take(']')) return {};
        while (true) {
            skip_value(cursor);
            if (cursor.try_take(',')) continue;
            cursor.expect(']');
            return {};
        }
    }
    throw std::runtime_error("json: cannot skip value");
}

struct ReferenceCase {
    std::string name;
    std::string text;
    std::int64_t tokens;
};

std::vector<ReferenceCase> parse_reference(std::string_view json) {
    JsonCursor cursor(json);
    std::vector<ReferenceCase> cases;
    cursor.expect('{');
    if (cursor.try_take('}')) return cases;
    while (true) {
        ReferenceCase entry;
        entry.name = cursor.parse_string();
        cursor.expect(':');
        cursor.expect('{');
        bool have_text = false;
        bool have_tokens = false;
        while (true) {
            const std::string field = cursor.parse_string();
            cursor.expect(':');
            if (field == "text") {
                entry.text = cursor.parse_string();
                have_text = true;
            } else if (field == "tokens") {
                entry.tokens = cursor.parse_int();
                have_tokens = true;
            } else {
                skip_value(cursor);
            }
            if (cursor.try_take(',')) continue;
            cursor.expect('}');
            break;
        }
        if (!have_text || !have_tokens) {
            throw std::runtime_error("json: incomplete reference case " + entry.name);
        }
        cases.push_back(std::move(entry));
        if (cursor.try_take(',')) continue;
        cursor.expect('}');
        break;
    }
    return cases;
}

}  // namespace

TEST(o200k_vocabulary_loads_completely) {
    const std::string content = read_file(AGORA_FIXTURE_VOCABULARY);
    const agora::core::BpeVocabulary vocabulary = agora::core::BpeVocabulary::parse_tiktoken(content);
    // o200k_base publishes 199998 mergeable ranks; a short load would silently overcount.
    CHECK_EQ(std::size_t(199998), vocabulary.size());
}

TEST(o200k_reference_texts_count_exactly_as_tiktoken_does) {
    const std::string vocabulary_content = read_file(AGORA_FIXTURE_VOCABULARY);
    const agora::core::BpeVocabulary vocabulary =
        agora::core::BpeVocabulary::parse_tiktoken(vocabulary_content);
    const std::vector<ReferenceCase> cases = parse_reference(read_file(AGORA_FIXTURE_REFERENCE));
    CHECK(cases.size() >= 20);
    for (const ReferenceCase& reference : cases) {
        const std::int64_t counted = agora::core::bpe_token_count(reference.text, vocabulary);
        if (counted != reference.tokens) {
            agora::core::test::fail(
                __FILE__,
                __LINE__,
                reference.name + ": expected " + std::to_string(reference.tokens) +
                    " but got " + std::to_string(counted) + " for: " + reference.text
            );
        }
    }
}
