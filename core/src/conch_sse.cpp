#include "agora/core/conch_sse.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "agora/core/json.hpp"

namespace agora::core::conch {
namespace {

using json::Kind;
using json::Value;

constexpr int kMaxWarningChars = 8192;

[[noreturn]] void fail(std::string reason) {
    throw std::runtime_error("Invalid Conch SSE stream: " + reason);
}

bool supported_event(std::string_view event) {
    return event == "line" || event == "warning" || event == "result" || event == "error";
}

bool output_stream(std::string_view stream) { return stream == "stdout" || stream == "stderr"; }

/** One decoded UTF-8 code point and how many bytes it occupied. */
struct CodePoint {
    char32_t value = 0;
    int width = 0;  // bytes
};

std::optional<CodePoint> decode_at(std::string_view text, std::size_t at) {
    if (at >= text.size()) return std::nullopt;
    const auto first = static_cast<unsigned char>(text[at]);
    int width = 0;
    char32_t value = 0;
    if (first < 0x80) {
        width = 1;
        value = first;
    } else if ((first & 0xe0) == 0xc0) {
        width = 2;
        value = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        width = 3;
        value = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        width = 4;
        value = first & 0x07;
    } else {
        width = 1;  // stray continuation byte: count it, never split on it
        value = first;
    }
    if (at + static_cast<std::size_t>(width) > text.size()) width = 1, value = first;
    for (int i = 1; i < width; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xc0) != 0x80) width = 1, value = first;
        value = (value << 6) | (next & 0x3f);
    }
    return CodePoint{value, width};
}

// ---------------------------------------------------------------------------------------------
// Java String semantics the parser relies on
// ---------------------------------------------------------------------------------------------

/**
 * Kotlin's Char.isWhitespace() = Character.isWhitespace(c) || Character.isSpaceChar(c), which is
 * what both String.trim() and String.isBlank() use. The union covers the ISO control separators,
 * NEL, the Zs/Zl/Zp space separators (so U+00A0 and U+2007 ARE trimmed, unlike plain Java
 * Character.isWhitespace) and the U+2000..U+200A block.
 */
bool kotlin_whitespace(char32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\v' || cp == '\f' || cp == '\r' || cp == ' ') return true;
    if (cp >= 0x1c && cp <= 0x1f) return true;
    if (cp == 0x85) return true;
    if (cp == 0x00a0) return true;
    if (cp == 0x1680) return true;
    if (cp >= 0x2000 && cp <= 0x200a) return true;
    if (cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000) return true;
    return false;
}

/** kotlin String.isNotBlank(): at least one character is not whitespace. */
bool not_blank(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const auto cp = decode_at(text, at);
        if (!cp) return true;
        if (!kotlin_whitespace(cp->value)) return true;
        at += cp->width;
    }
    return false;
}

/** kotlin String.trim(): drops leading and trailing whitespace. */
std::string kotlin_trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end) {
        const auto cp = decode_at(text, begin);
        if (!cp || !kotlin_whitespace(cp->value)) break;
        begin += cp->width;
    }
    while (end > begin) {
        std::size_t probe = begin;
        std::optional<CodePoint> last;
        while (probe < end) {
            auto cp = decode_at(text, probe);
            if (!cp) break;
            last = cp;
            probe += cp->width;
        }
        if (!last || !kotlin_whitespace(last->value)) break;
        end -= last->width;
    }
    return std::string(text.substr(begin, end - begin));
}

/** kotlin String.take(n) over UTF-16 units; scenarios only reach ASCII so bytes are equivalent. */
std::string take_chars(std::string_view text, int limit) {
    if (limit <= 0) return "";
    if (static_cast<std::size_t>(limit) >= text.size()) return std::string(text);
    return std::string(text.substr(0, static_cast<std::size_t>(limit)));
}

// ---------------------------------------------------------------------------------------------
// kotlin toIntOrNull / toLongOrNull / toBooleanStrictOrNull
// ---------------------------------------------------------------------------------------------

std::optional<std::int64_t> parse_long(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::size_t at = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        at = 1;
    }
    if (at >= text.size()) return std::nullopt;
    std::uint64_t magnitude = 0;
    const std::uint64_t limit = negative ? 9223372036854775808ull : 9223372036854775807ull;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        if (c < '0' || c > '9') return std::nullopt;
        if (magnitude > (limit - static_cast<std::uint64_t>(c - '0')) / 10) return std::nullopt;
        magnitude = magnitude * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (negative) return -static_cast<std::int64_t>(magnitude);
    return static_cast<std::int64_t>(magnitude);
}

std::optional<int> to_int(std::string_view text) {
    const auto parsed = parse_long(text);
    if (!parsed) return std::nullopt;
    if (*parsed < -2147483648ll || *parsed > 2147483647ll) return std::nullopt;
    return static_cast<int>(*parsed);
}

std::optional<bool> to_boolean_strict(std::string_view text) {
    if (text == "true") return true;
    if (text == "false") return false;
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// JsonObject field accessors (the private helpers at the bottom of ConchSseParser.kt)
// ---------------------------------------------------------------------------------------------

const Value* lookup(const Value& data, std::string_view key) {
    for (const json::Member& member : data.members) {
        if (member.key == key) return &member.value;
    }
    return nullptr;
}

std::string required_string(const Value& data, std::string_view key, std::string_view event,
                            bool allow_blank = false) {
    const Value* value = lookup(data, key);
    if (value == nullptr || value->kind != Kind::String ||
        (!allow_blank && !not_blank(value->text))) {
        fail("event " + std::string(event) + " requires string " + std::string(key));
    }
    return value->text;
}

int required_int(const Value& data, std::string_view key, std::string_view event) {
    const Value* value = lookup(data, key);
    if (value == nullptr || value->kind == Kind::String) {
        fail("event " + std::string(event) + " requires integer " + std::string(key));
    }
    const auto parsed = to_int(value->text);
    if (!parsed) {
        fail("event " + std::string(event) + " requires integer " + std::string(key));
    }
    return *parsed;
}

std::optional<bool> optional_boolean(const Value& data, std::string_view key,
                                     std::string_view event) {
    const Value* value = lookup(data, key);
    if (value == nullptr) return std::nullopt;
    if (value->kind == Kind::String) {
        fail("event " + std::string(event) + " requires boolean " + std::string(key));
    }
    const auto parsed = to_boolean_strict(value->text);
    if (!parsed) {
        fail("event " + std::string(event) + " requires boolean " + std::string(key));
    }
    return parsed;
}

}  // namespace

std::string shell_utf8_prefix(std::string_view text, int max_bytes) {
    if (max_bytes <= 0) return "";
    std::size_t at = 0;
    int used = 0;
    while (at < text.size()) {
        const auto cp = decode_at(text, at);
        if (!cp) break;
        if (used + cp->width > max_bytes) break;
        used += cp->width;
        at += cp->width;
    }
    return std::string(text.substr(0, at));
}

SseResult parse_conch_sse_lines(bool encrypted, const LineReader& read_line,
                                const Decrypter& decrypt, const OutputSink& on_output) {
    std::string output;
    int output_bytes = 0;
    std::optional<int> exit_code;
    std::optional<std::string> error_message;
    bool timed_out = false;
    std::optional<std::string> warning_message;
    bool output_truncation_reported = false;
    std::optional<std::string> current_event;
    bool terminal_seen = false;
    std::int64_t expected_sequence = 0;

    const auto record_warning = [&](std::string message) {
        const std::string bounded = take_chars(message, kMaxWarningChars);
        if (!warning_message) {
            warning_message = bounded;
        } else {
            *warning_message = take_chars(*warning_message + "\n" + bounded, kMaxWarningChars);
        }
    };

    const auto record_output_truncation = [&]() {
        if (output_truncation_reported) return;
        output_truncation_reported = true;
        const std::string notice =
            "Conch output was truncated at " + std::to_string(kCommandOutputMaxBytes) +
            " UTF-8 bytes.";
        if (!warning_message) {
            warning_message = notice;
        } else {
            *warning_message = take_chars(notice + "\n" + *warning_message, kMaxWarningChars);
        }
    };

    while (true) {
        const auto line = read_line();
        if (!line) break;

        if (line->empty()) {
            if (current_event) fail("event " + *current_event + " has no data frame");
        } else if (line->rfind("event: ", 0) == 0) {
            if (terminal_seen) fail("event received after terminal frame");
            if (current_event) fail("event " + *current_event + " has no data frame");
            const std::string event = kotlin_trim(line->substr(7));
            if (!supported_event(event)) fail("unsupported event " + event);
            current_event = event;
        } else if (line->rfind("data: ", 0) == 0) {
            if (terminal_seen) fail("data received after terminal frame");
            if (!current_event) fail("data frame has no event");
            const std::string event = *current_event;
            current_event.reset();

            std::string data_text = kotlin_trim(line->substr(6));
            if (encrypted) {
                try {
                    data_text = decrypt(data_text);
                } catch (const std::exception& error) {
                    fail(std::string("decryption failed: ") + error.what());
                }
            }

            const json::ParseResult parsed = json::parse(data_text);
            if (!parsed.value || parsed.value->kind != Kind::Object) {
                fail("event " + event + " contains invalid JSON");
            }
            const Value& data = *parsed.value;

            if (encrypted) {
                // Kotlin evaluates requiredString first, so a missing or non-string _conch_event
                // raises "requires string _conch_event" rather than the mismatch message.
                const bool event_matches =
                    required_string(data, "_conch_event", event) == event;
                const Value* sequence = lookup(data, "_conch_sequence");
                const bool sequence_ok =
                    sequence != nullptr && sequence->kind == Kind::Literal &&
                    parse_long(sequence->text) &&
                    *parse_long(sequence->text) == expected_sequence &&
                    expected_sequence != INT64_MAX;
                if (!event_matches || !sequence_ok) {
                    fail("event authentication or sequence mismatch");
                }
                expected_sequence += 1;
            }

            if (event == "line") {
                const std::string text = required_string(data, "line", event, /*allow_blank=*/true);
                const std::string stream = required_string(data, "stream", event);
                if (!output_stream(stream)) fail("event line has unsupported stream " + stream);

                const std::string raw_delta = text + "\n";
                const std::string prefix =
                    shell_utf8_prefix(raw_delta, kCommandOutputMaxBytes - output_bytes);
                if (!prefix.empty()) {
                    output += prefix;
                    output_bytes += static_cast<int>(prefix.size());
                    on_output(prefix);
                }
                if (prefix.size() < raw_delta.size()) record_output_truncation();
            } else if (event == "warning") {
                record_warning(required_string(data, "message", event));
            } else if (event == "result") {
                exit_code = required_int(data, "exit_code", event);
                terminal_seen = true;
            } else if (event == "error") {
                error_message = required_string(data, "message", event);
                timed_out = optional_boolean(data, "timed_out", event).value_or(false);
                terminal_seen = true;
            }
        } else {
            fail("unexpected line");
        }
    }

    if (current_event) fail("event " + *current_event + " has no data frame");
    if (!terminal_seen) fail("stream ended without a terminal result or error");

    if (output.size() >= 1 && output.back() == '\n') output.pop_back();

    SseResult result;
    result.output = std::move(output);
    result.exit_code = exit_code;
    result.error_message = error_message;
    result.timed_out = timed_out;
    result.warning_message = warning_message;
    return result;
}

}  // namespace agora::core::conch
