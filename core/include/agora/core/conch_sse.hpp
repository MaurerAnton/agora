#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace agora::core::conch {

/** Port of ConchSseParser's ConchSseResult. */
struct SseResult {
    std::string output;                     // streamed output with the trailing newline removed
    std::optional<int> exit_code;           // set by a terminal `result` frame
    std::optional<std::string> error_message;  // set by a terminal `error` frame
    bool timed_out = false;
    std::optional<std::string> warning_message;
};

/** Reads the next raw SSE line; std::nullopt signals end of input. */
using LineReader = std::function<std::optional<std::string>()>;
/** Reverses the transport cipher; may throw, which fails the parse closed. */
using Decrypter = std::function<std::string(const std::string&)>;
/** Receives each output delta as soon as it is decoded — before the terminal frame arrives. */
using OutputSink = std::function<void(const std::string&)>;

/**
 * Native port of tool/ConchSseParser.kt's parseConchSseLines: the strict one-event/one-data SSE
 * protocol used by the Conch shell backend.
 *
 * Streaming output is emitted before the terminal frame arrives, but malformed, unknown,
 * incomplete, duplicate-terminal and post-terminal input all fail closed with the message the
 * Kotlin produces — every one of those messages is pinned by the golden traces. Retained and
 * emitted output share one byte budget, so a truncated stream stops emitting mid-word.
 *
 * @throws std::runtime_error prefixed with "Invalid Conch SSE stream: " on any violation.
 */
SseResult parse_conch_sse_lines(bool encrypted, const LineReader& read_line,
                                const Decrypter& decrypt, const OutputSink& on_output);

/**
 * Port of ShellClient.kt's String.shellUtf8Prefix: the longest prefix of @p text that fits into
 * @p max_bytes of UTF-8, never splitting a code point. Returns an empty string when nothing fits.
 */
std::string shell_utf8_prefix(std::string_view text, int max_bytes);

/** ShellClient.kt's SHELL_COMMAND_OUTPUT_MAX_BYTES (1 shl 20). */
inline constexpr int kCommandOutputMaxBytes = 1 << 20;

}  // namespace agora::core::conch
