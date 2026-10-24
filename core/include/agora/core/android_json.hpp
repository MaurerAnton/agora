#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::android_json {

/**
 * Ports of android.util.JsonReader / android.util.JsonWriter (AOSP frameworks/base), vendored
 * verbatim by the `native-conversation-graph-source` golden stand (vendor/android-util-json) —
 * the stand's frozen traces pin this port against that exact Android code.
 *
 * Contract notes (measured, not assumed):
 *  - The reader is always strict (lenient = false): unquoted names/literals, comments and '='
 *    separators raise MalformedJsonException with Android's " at line L column C" suffix,
 *    where the column counts UTF-16 units since the line start (computed here over the UTF-8
 *    bytes so astral characters count as two, exactly like the Java char buffer).
 *  - Raw control characters INSIDE string literals are accepted and carried through; the writer
 *    re-escapes them (plus U+2028/U+2029) as backslash-u lowercase-hex escapes.
 *  - EOF while a value is still expected raises EOFException "End of input" (no position);
 *    EOF after a complete top-level value returns Token::EndDocument.
 *  - value(double) emission is Java's Double.toString (shortest roundtrip, "1.0E10"-style
 *    scientific form for decimal exponents < -3 or >= 7), and non-finite doubles raise
 *    IllegalArgumentException "Numeric values must be finite, but was <Double.toString>".
 *    Verified against the JDK's Double.toString over 700+ probes (randoms, boundary normals,
 *    subnormals); the ONE divergence class is the smallest denormals, where Java keeps an extra
 *    digit its own parser does not need (Double.MIN_VALUE prints "4.9E-324", the port prints the
 *    shortest roundtrip "5.0E-324" — likewise for denormals with bits 10..16). Subnormal values
 *    are outside the frozen set and outside any archive the app produces; this is the pinned
 *    boundary of java_double_to_string.
 *  - Buffer size is 1024 like the AOSP reader; refills are transparent for every observable
 *    surface (content, error text, line/column) because the line/column bookkeeping happens at
 *    refill exactly like AOSP's fillBuffer.
 *
 * Malformed backslash-u escapes raise the reference NumberFormatException class with a
 * best-effort message; that input shape is not part of the frozen set.
 */

/**
 * Any java.io/java.lang exception the streaming layer raises. java_class() is the simple class
 * name the golden drivers record ("MalformedJsonException", "EOFException", ...).
 */
class Exception : public std::runtime_error {
public:
    Exception(std::string java_class, std::string message)
        : std::runtime_error(std::move(message)), java_class_(std::move(java_class)) {}

    const std::string& java_class() const noexcept { return java_class_; }

private:
    std::string java_class_;
};

/**
 * The java.io.InputStream surface a Reader reads from: read() returns -1 at the end and may
 * throw (the checked-archive failure the stand's drain-fault double raises).
 */
class Source {
public:
    virtual ~Source() = default;
    virtual long long read(char* buffer, long long capacity) = 0;
    /** java InputStream.close(); idempotent implementations mirror the JDK contract. */
    virtual void close() {}
};

/** The extracted app helper InputStream.readToEnd() — drains so a checked stream verifies. */
void read_to_end(Source& source);

/** android.util.JsonToken. */
enum class Token {
    BeginArray,
    EndArray,
    BeginObject,
    EndObject,
    Name,
    String,
    Number,
    Boolean,
    Null,
    EndDocument,
};

/** The JsonToken enum constant names as Android spells them in messages. */
std::string token_name(Token token);

/**
 * android.util.JsonReader, strict mode, over UTF-8 bytes. The app uses exactly this surface
 * (beginObject/endObject/hasNext/nextName/beginArray/endArray/skipValue/peek/nextString/
 * nextBoolean/nextNull); values and names are UTF-8 strings with backslash escapes decoded
 * (surrogate pairs combined into the corresponding UTF-8 sequence).
 */
class Reader {
public:
    explicit Reader(Source& source);

    void begin_array();
    void end_array();
    void begin_object();
    void end_object();
    bool has_next();
    Token peek();
    std::string next_name();
    std::string next_string();
    bool next_boolean();
    void next_null();
    void skip_value();

private:
    enum class Scope {
        EmptyDocument,
        NonemptyDocument,
        EmptyArray,
        NonemptyArray,
        EmptyObject,
        NonemptyObject,
        DanglingName,
        Closed,
    };

    static constexpr std::size_t kBufferSize = 1024;

    Scope stack_top() const;
    void push(Scope kind);
    Scope pop();
    void replace_top(Scope kind);

    int next_non_whitespace();
    void check_lenient();
    void skip_to_end_of_line();
    bool skip_to(std::string_view to_find);
    /** True when limit_ (post-compaction) reached `minimum` — AOSP fillBuffer's boolean. */
    bool fill_buffer(std::size_t minimum);
    std::size_t line_number() const;
    std::size_t column_number() const;

    Token next_value();
    Token next_in_array(bool first_element);
    Token next_in_object(bool first_element);
    Token object_value();
    Token read_literal();
    Token decode_literal();
    static Token decode_number(const std::string& literal);
    std::string read_string(char quote);
    std::string next_literal(bool assign_offsets_only);
    char read_escape_character();
    Exception syntax_error(std::string_view message);

    /**
     * Appends one decoded codepoint (or a U+FFFD replacement for a malformed sequence, UAX-19
     * maximal-subpart style, matching Java's UTF_8 CharsetDecoder with REPLACE) to out_.
     * Returns false when the sequence is incomplete and more input is needed.
     */
    static bool decode_one(const char* data, std::size_t size, bool at_eof, std::string& out,
                           std::size_t& consumed);

    Source& in_;
    std::vector<char> buffer_;  // kBufferSize + 1: AOSP writes a '\0' sentinel at buffer[limit]
    std::string carry_;         // raw bytes read but not yet placed (never splits a codepoint)
    std::uint32_t high_surrogate_ = 0;  // pending backslash-u high half inside read_string
    bool source_eof_ = false;
    std::size_t pos_ = 0;
    std::size_t limit_ = 0;
    std::size_t buffer_start_line_ = 1;
    std::size_t buffer_start_column_ = 1;
    std::vector<Scope> stack_;
    Token token_ = Token::EndDocument;
    bool token_set_ = false;
    std::string name_;
    std::string value_;
    bool has_value_ = false;
};

/**
 * android.util.JsonWriter in its compact configuration (no setIndent, strict). Emits into an
 * internal byte buffer with Android's escape rules: quote, backslash, tab, backspace, newline,
 * carriage return, form feed, U+2028/U+2029 and every control <= 0x1F as lowercase-hex
 * backslash-u escapes; everything else raw UTF-8.
 */
class Writer {
public:
    Writer() = default;

    void begin_array();
    void end_array();
    void begin_object();
    void end_object();
    void name(std::string_view name);
    void value_null();
    void value_bool(bool value);
    void value_long(long long value);
    void value_double(double value);
    void value_string(std::string_view value);

    /** Android's close(): flushes, then raises IOException("Incomplete document") if unbalanced. */
    void close();

    const std::string& str() const { return out_; }

private:
    enum class Scope {
        EmptyDocument,
        NonemptyDocument,
        EmptyArray,
        NonemptyArray,
        EmptyObject,
        NonemptyObject,
        DanglingName,
    };

    Scope peek() const;
    void replace_top(Scope scope);
    void open(Scope empty, std::string_view bracket);
    void close_scope(Scope empty, Scope nonempty, std::string_view bracket);
    void before_name();
    void before_value(bool root);
    void write_string(std::string_view value);

    std::string out_;
    std::vector<Scope> stack_{Scope::EmptyDocument};
};

/** java.lang.Long.toString(long). */
std::string java_long_to_string(long long value);

/** java.lang.Double.toString(double) — shortest roundtrip with Java's decimal/scientific rule. */
std::string java_double_to_string(double value);

/** java.lang.Double.parseDouble (decimal/exponent literals as the reader classifies them). */
double java_parse_double(std::string_view text);

}  // namespace agora::core::android_json
