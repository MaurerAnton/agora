#include "agora/core/shell_file_edit.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "agora/core/sha256.hpp"
#include "agora/core/java_utf8.hpp"

namespace agora::core::shell_file {
namespace {

constexpr std::u16string_view kCrlf = u"\r\n";
constexpr std::u16string_view kLf = u"\n";

/** Kotlin String.replace("\r\n", "\n"): left-to-right, non-overlapping. */
std::u16string normalize_crlf(std::u16string_view text) {
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') {
            out.push_back(u'\n');
            i += 2;
        } else {
            out.push_back(text[i]);
            ++i;
        }
    }
    return out;
}

std::string hex_lower(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    const std::array<std::uint8_t, 32> digest = sha256(bytes);
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

}  // namespace

std::string shell_file_sha256(std::string_view content) { return hex_lower(content); }

std::vector<Utf16Range> file_edit_match_ranges(std::u16string_view content,
                                               std::u16string_view old_string) {
    if (old_string.empty()) throw std::invalid_argument("Failed requirement.");

    std::u16string normalized_content;
    normalized_content.reserve(content.size());
    std::vector<std::size_t> original_boundaries(content.size() + 1);
    std::size_t original_index = 0;
    std::size_t normalized_length = 0;
    original_boundaries[0] = 0;
    while (original_index < content.size()) {
        if (content[original_index] == u'\r' && original_index + 1 < content.size() &&
            content[original_index + 1] == u'\n') {
            normalized_content.push_back(u'\n');
            original_index += 2;
        } else {
            normalized_content.push_back(content[original_index]);
            original_index += 1;
        }
        normalized_length += 1;
        original_boundaries[normalized_length] = original_index;
    }

    const std::u16string normalized_old_string = normalize_crlf(old_string);
    std::vector<Utf16Range> matches;
    std::size_t search_index = 0;
    while (search_index + normalized_old_string.size() <= normalized_content.size()) {
        const std::size_t normalized_start =
            normalized_content.find(normalized_old_string, search_index);
        if (normalized_start == std::u16string::npos) break;
        const std::size_t normalized_end = normalized_start + normalized_old_string.size();
        matches.push_back({original_boundaries[normalized_start],
                           original_boundaries[normalized_end] - 1});
        search_index = normalized_end;
    }
    return matches;
}

std::u16string replace_file_edit_matches(std::u16string_view content,
                                         const std::vector<Utf16Range>& matches,
                                         std::u16string_view new_string) {
    if (matches.empty()) return std::u16string(content);

    const std::optional<std::u16string> file_line_ending =
        preferred_file_edit_line_ending(content);
    std::u16string result;
    result.reserve(content.size());
    std::size_t source_index = 0;
    for (const Utf16Range& match : matches) {
        if (!(match.first >= source_index && match.last < content.size())) {
            throw std::invalid_argument("Failed requirement.");
        }
        result.append(content.substr(source_index, match.first - source_index));
        // Kotlin substring(IntRange) uses last + 1 as the exclusive end; an "empty" range
        // (last + 1 == first) is legal, a reversed one throws from substring.
        const std::size_t matched_end = match.last + 1;
        if (matched_end < match.first) throw std::invalid_argument("Failed requirement.");
        const std::u16string_view matched_text =
            content.substr(match.first, matched_end - match.first);
        std::optional<std::u16string> line_ending =
            preferred_file_edit_line_ending(matched_text);
        if (!line_ending) line_ending = file_line_ending;
        if (!line_ending) line_ending = preferred_file_edit_line_ending(new_string);
        if (!line_ending) line_ending = std::u16string(kLf);
        result.append(with_file_edit_line_ending(new_string, *line_ending));
        source_index = match.last + 1;
    }
    result.append(content.substr(source_index));
    return result;
}

std::pair<std::optional<std::u16string>, ShellFileEditResult> edit_shell_file_content(
    std::u16string_view content, std::u16string_view old_string, std::u16string_view new_string,
    bool replace_all) {
    const std::vector<Utf16Range> matches = file_edit_match_ranges(content, old_string);
    if (matches.empty()) {
        return {std::nullopt, {0, "", "old_string not found in file"}};
    }
    if (matches.size() > 1 && !replace_all) {
        return {std::nullopt,
                {0, "",
                 "found " + std::to_string(matches.size()) +
                     " matches of old_string; set replace_all=true or provide a unique match"}};
    }
    std::vector<Utf16Range> selected_matches;
    if (replace_all) {
        selected_matches = matches;
    } else {
        selected_matches.push_back(matches.front());
    }
    std::u16string replaced = replace_file_edit_matches(content, selected_matches, new_string);
    return {std::move(replaced),
            {static_cast<int>(selected_matches.size()), "", std::nullopt}};
}

std::optional<std::u16string> preferred_file_edit_line_ending(std::u16string_view text) {
    int crlf_count = 0;
    int lf_count = 0;
    std::optional<std::u16string> first_line_ending;
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] == u'\r' && index + 1 < text.size() && text[index + 1] == u'\n') {
            crlf_count += 1;
            if (!first_line_ending) first_line_ending = std::u16string(kCrlf);
            index += 2;
        } else {
            if (text[index] == u'\n') {
                lf_count += 1;
                if (!first_line_ending) first_line_ending = std::u16string(kLf);
            }
            index += 1;
        }
    }
    if (crlf_count > lf_count) return std::u16string(kCrlf);
    if (lf_count > crlf_count) return std::u16string(kLf);
    return first_line_ending;
}

std::u16string with_file_edit_line_ending(std::u16string_view text,
                                          std::u16string_view line_ending) {
    if (text.find(u'\n') == std::u16string_view::npos) return std::u16string(text);
    const std::u16string normalized = normalize_crlf(text);
    if (line_ending == kLf) return normalized;
    std::u16string out;
    out.reserve(normalized.size());
    for (const char16_t unit : normalized) {
        if (unit == u'\n') {
            out.append(line_ending);
        } else {
            out.push_back(unit);
        }
    }
    return out;
}

std::size_t complete_utf8_prefix_length(std::string_view bytes) {
    if (bytes.empty()) return 0;
    std::size_t lead_index = bytes.size() - 1;
    while ((static_cast<unsigned char>(bytes[lead_index]) & 0xC0) == 0x80) {
        if (lead_index == 0) return 0;
        --lead_index;
    }
    const auto lead = static_cast<unsigned char>(bytes[lead_index]);
    std::size_t expected_bytes = 1;
    if ((lead & 0x80) == 0) {
        expected_bytes = 1;
    } else if ((lead & 0xE0) == 0xC0) {
        expected_bytes = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        expected_bytes = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        expected_bytes = 4;
    } else {
        expected_bytes = 1;
    }
    return bytes.size() - lead_index < expected_bytes ? lead_index : bytes.size();
}

std::pair<std::u16string, bool> read_bounded_shell_output(std::string_view bytes, int max_bytes) {
    if (max_bytes < 0) throw std::invalid_argument("Failed requirement.");
    const std::size_t limit = static_cast<std::size_t>(max_bytes);
    const std::size_t retained_size = std::min(bytes.size(), limit);
    const bool truncated = bytes.size() > limit;
    const std::string_view retained = bytes.substr(0, retained_size);
    const std::size_t complete_bytes =
        truncated ? complete_utf8_prefix_length(retained) : retained.size();
    std::u16string content;
    content.reserve(complete_bytes);
    java_utf8::decode_replace(retained.substr(0, complete_bytes), content);
    return {std::move(content), truncated};
}

}  // namespace agora::core::shell_file
