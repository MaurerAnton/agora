#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::shell_file {

// The pure top-of-file helpers from util/ShellClient.kt. They own the file-edit matching and
// replacement policy, line counting, the SHA-256 reported by the edit tool, and the bounded
// shell-output retention/decode rule. Sockets, file IO and the process stream itself remain the
// platform boundary; `read_bounded_shell_output` takes the already-collected byte stream.

// ShellFileReadResult and shellFileLineCount live in shell_tools.hpp: the shell-tools stand
// already extracts and pins them from ShellClient.kt, so they are not duplicated here.

/** Kotlin ShellFileEditResult. */
struct ShellFileEditResult {
    int replacements = 0;
    std::string sha256;
    std::optional<std::string> error;
};

/** Kotlin IntRange: both ends inclusive. Ranges produced here are non-negative and ordered. */
struct Utf16Range {
    std::size_t first = 0;
    std::size_t last = 0;
};

/** shellFileSha256: SHA-256 of the bytes, lowercase hex (`"%02x"` per unsigned byte). */
std::string shell_file_sha256(std::string_view content);

/**
 * fileEditMatchRanges.
 *
 * CRLF in both the content and the needle is normalized to LF before searching; the returned
 * ranges point into the original UTF-16 string and include a matched CRLF pair as two units.
 * Matches are non-overlapping (search resumes after the previous match). An empty needle throws
 * std::invalid_argument("Failed requirement.") like Kotlin `require`.
 */
std::vector<Utf16Range> file_edit_match_ranges(std::u16string_view content,
                                               std::u16string_view old_string);

/**
 * replaceFileEditMatches.
 *
 * `matches` are inclusive, ordered ranges into `content`. Each replacement picks the line ending
 * of the matched text, then the file, then the replacement text, then "\n"; the replacement is
 * normalized to that ending. A range that starts before the previous range ended or reaches past
 * the content throws std::invalid_argument("Failed requirement.").
 */
std::u16string replace_file_edit_matches(std::u16string_view content,
                                         const std::vector<Utf16Range>& matches,
                                         std::u16string_view new_string);

/**
 * editShellFileContent. Returns the edited text (nullopt when the edit was rejected) and the
 * result carrying the replacement count or the exact Kotlin error string.
 */
std::pair<std::optional<std::u16string>, ShellFileEditResult> edit_shell_file_content(
    std::u16string_view content, std::u16string_view old_string, std::u16string_view new_string,
    bool replace_all);

/** preferredFileEditLineEnding: majority line ending, ties use the first one, none -> nullopt. */
std::optional<std::u16string> preferred_file_edit_line_ending(std::u16string_view text);

/** withFileEditLineEnding: no LF -> unchanged; otherwise CRLF-normalize then apply the ending. */
std::u16string with_file_edit_line_ending(std::u16string_view text,
                                          std::u16string_view line_ending);

/**
 * completeUtf8PrefixLength: if the trailing bytes are an incomplete UTF-8 sequence, the length
 * before that sequence; otherwise the full length. A continuation-only buffer returns 0.
 */
std::size_t complete_utf8_prefix_length(std::string_view bytes);

/**
 * readBoundedShellOutput over a byte stream that has already been collected.
 *
 * Retains at most `max_bytes`, reports truncation when the stream was longer, drops an
 * incomplete trailing UTF-8 sequence only when truncated, and decodes the rest with the JDK
 * REPLACE-mode decoder (one U+FFFD per malformed maximal subpart). A negative `max_bytes`
 * throws std::invalid_argument("Failed requirement.").
 */
std::pair<std::u16string, bool> read_bounded_shell_output(std::string_view bytes, int max_bytes);

}  // namespace agora::core::shell_file
