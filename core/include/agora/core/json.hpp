#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::json {

/**
 * A parsed JSON value with kotlinx.serialization acceptance rules.
 *
 * This is the syntactic half of the pinned reference decoder (see
 * core/tools/golden/openai): it accepts exactly what the reference lexer
 * accepts — raw control bytes inside strings, unquoted runs of "TC_OTHER"
 * characters without the RFC 8259 number grammar — and rejects what it
 * rejects (BOM, trailing commas, trailing content, invalid escapes).
 *
 * Type-level rules (Int literals, known-field types) live in the model walk,
 * which re-scans the raw source span in [Value::begin] exactly like
 * AbstractJsonLexer.consumeNumericLiteral.
 */
enum class Kind {
    String,   // quoted string; text holds the decoded content
    Literal,  // unquoted TC_OTHER run; text holds the raw characters
    Array,
    Object,
};

struct Value;
struct Member;

struct Value {
    Kind kind = Kind::Literal;
    std::string text;
    std::vector<Value> elements;  // Array
    std::vector<Member> members;  // Object: source order, duplicates kept
    std::size_t begin = 0;        // offset of the token start in the source
    std::size_t end = 0;          // offset one past the token

    bool is_literal(std::string_view raw) const {
        return kind == Kind::Literal && text == raw;
    }
};

/** One object member; defined after Value because it stores a complete Value. */
struct Member {
    std::string key;
    Value value;
};

struct ParseResult {
    std::optional<Value> value;
    std::size_t error_position = 0;
    std::string error;
};

/** Parses one complete JSON document exactly like kotlinx Json.decodeFromString. */
ParseResult parse(std::string_view source);

/** kotlinx StringOps escaping, as pinned by transcript 77 (lowercase \u00xx). */
void escape_to(std::string& out, std::string_view text);

/** kotlinx JsonElement.toString(): canonical compact re-serialization, last
 *  duplicate key wins but keeps its first position (LinkedHashMap). */
std::string to_kotlin_string(const Value& value);

/**
 * Kotlin Character.isWhitespace(c) || Character.isSpaceChar(c): the set
 * String.trim(), isBlank() and friends operate on.
 */
bool is_kotlin_whitespace_code_point(std::uint32_t code_point);

/** Kotlin String.trim(). */
std::string trim(std::string_view text);

/** Kotlin String.isBlank(). */
bool is_blank(std::string_view text);

/** Kotlin String.take(units) counted in UTF-16 code units; a split surrogate
 *  pair becomes '?' on output, matching the JVM UTF-8 encoder. */
std::string take_utf16_units(std::string_view text, std::size_t units);

/** Kotlin String.length: UTF-16 code units (astral code points count as two). */
std::size_t utf16_length(std::string_view text);

/** charToTokenClass(c) == TC_OTHER for a raw byte, as used by the scanner. */
bool is_other_token_byte(unsigned char byte);

}  // namespace agora::core::json
