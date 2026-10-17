#include "agora/core/citations.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/java_uri.hpp"
#include "agora/core/sha256.hpp"
#include "agora/core/unicode.hpp"
#include "jvm_pow10_table.generated.hpp"
#include "utf16.hpp"

// Port of the citation pipeline (CitationPolicy + AnthropicCitationProjection +
// GeminiCitationProjection). The private-marker patterns below are small enough that a tiny
// hand-rolled matcher with Java's leftmost-first greedy semantics is a closer port than
// std::regex (which also differs on '$' and would be a heavy dependency here).

namespace agora::core::citations {
namespace {

using U16 = std::u16string;
constexpr std::size_t kNpos = std::u16string::npos;

// ---------------------------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------------------------

/** Kotlin String.lowercase() restricted to ASCII (see the header's boundary note). */
std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/** Kotlin String.contains(other, ignoreCase = true) with ASCII folding. */
bool contains_ignore_case(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const auto fold = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() &&
               fold(static_cast<unsigned char>(haystack[i + k])) ==
                   fold(static_cast<unsigned char>(needle[k]))) {
            k++;
        }
        if (k == needle.size()) return true;
    }
    return false;
}

/** CitationPolicy.bounded: empty becomes null; longer than maxChars is cut in UTF-16 units
 *  without splitting a surrogate pair. */
std::optional<std::string> bounded_plain(std::string_view value, std::size_t max_chars) {
    if (value.empty()) return std::nullopt;
    const std::u16string raw = utf16::from_utf8(value);
    if (raw.size() <= max_chars) return std::string(value);
    std::u16string cut = raw.substr(0, max_chars);
    // Kotlin: an end index right after a high surrogate whose next char is a low
    // surrogate moves back one, so a valid pair is never split.
    if (max_chars >= 1 && cut.back() >= 0xD800 && cut.back() <= 0xDBFF &&
        raw[max_chars] >= 0xDC00 && raw[max_chars] <= 0xDFFF) {
        cut.pop_back();
    }
    return utf16::to_utf8(cut);
}

std::optional<std::string> bounded(const std::optional<std::string>& value, std::size_t max_chars) {
    if (!value) return std::nullopt;
    return bounded_plain(*value, max_chars);
}

/** boundedMetadata(value) = bounded(value?.trim(), MAX_METADATA_CHARS). */
std::optional<std::string> bounded_metadata(const std::optional<std::string>& value) {
    if (!value) return std::nullopt;
    return bounded_plain(json::trim(*value), policy::kMaxMetadataChars);
}

/** Kotlin replaceFirstChar { it.uppercase() } with ASCII folding (see the header). */
std::string uppercase_first_char(const std::string& text) {
    if (text.empty()) return text;
    std::string out = text;
    const unsigned char first = static_cast<unsigned char>(out[0]);
    if (first >= 'a' && first <= 'z') out[0] = static_cast<char>(first - 'a' + 'A');
    return out;
}

/** MessageDigest SHA-256, hex encoded with "%02x" (lowercase). */
std::string sha256_hex(std::string_view value) {
    const std::array<std::uint8_t, 32> digest = sha256(value);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

/** Kotlin String.trimEnd(): strips trailing characters that isWhitespace() accepts. */
std::string trim_end(std::string_view text) {
    const std::u16string units = utf16::from_utf8(text);
    std::size_t end = units.size();
    while (end > 0 &&
           (unicode::flags_of(units[end - 1]) & (unicode::kJvmWhitespace | unicode::kJvmSpace)) !=
               0) {
        --end;
    }
    return utf16::to_utf8(units.substr(0, end));
}

/** CitationPolicy.escapeMarkdownLabel. */
std::string escape_markdown_label(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '[') {
            out += "\\[";
        } else if (c == ']') {
            out += "\\]";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/** URL.host with the "www." prefix removed, falling back to "Source". */
std::string url_host(const std::string& url) {
    std::string host;
    const std::optional<java_uri::Uri> parsed = java_uri::parse(url);
    if (parsed && parsed->host) host = *parsed->host;
    if (host.rfind("www.", 0) == 0) host.erase(0, 4);
    if (json::is_blank(host)) return "Source";
    return host;
}

// ---------------------------------------------------------------------------------------------
// The private-marker patterns (Java regex, IGNORE_CASE, leftmost-first greedy)
// ---------------------------------------------------------------------------------------------

constexpr char16_t kPua200 = 0xE200;
constexpr char16_t kPua201 = 0xE201;
constexpr char16_t kPua202 = 0xE202;

bool fold_eq(char16_t a, char16_t b) {
    const auto fold = [](char16_t c) -> char16_t {
        return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c - u'A' + u'a') : c;
    };
    return fold(a) == fold(b);
}

bool starts_with_fold(const U16& s, std::size_t pos, const char16_t* word) {
    std::size_t i = 0;
    while (word[i] != 0) {
        if (pos + i >= s.size() || !fold_eq(s[pos + i], word[i])) return false;
        ++i;
    }
    return true;
}

bool is_ascii_digit(char16_t c) { return c >= u'0' && c <= u'9'; }

bool is_ascii_alpha(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

/** The shared `turn\d+[a-z]+\d+` chunk; returns the end offset or kNpos. Greedy runs cannot
 *  help across chunk boundaries, so the match is deterministic. */
std::size_t match_turn_group(const U16& s, std::size_t pos) {
    const std::size_t n = s.size();
    if (!starts_with_fold(s, pos, u"turn")) return kNpos;
    std::size_t p = pos + 4;
    const std::size_t digits1 = p;
    while (p < n && is_ascii_digit(s[p])) ++p;
    if (p == digits1) return kNpos;
    const std::size_t letters = p;
    while (p < n && is_ascii_alpha(s[p])) ++p;
    if (p == letters) return kNpos;
    const std::size_t digits2 = p;
    while (p < n && is_ascii_digit(s[p])) ++p;
    if (p == digits2) return kNpos;
    return p;
}

using Match = std::optional<std::pair<std::size_t, std::size_t>>;

/** \uE200(?:cite|filecite)\uE202[^\uE200\uE201]+\uE201 */
Match find_private_envelope(const U16& s, std::size_t from) {
    const std::size_t n = s.size();
    for (std::size_t i = from; i < n; ++i) {
        if (s[i] != kPua200) continue;
        std::size_t p = i + 1;
        if (starts_with_fold(s, p, u"cite")) {
            p += 4;
        } else if (starts_with_fold(s, p, u"filecite")) {
            p += 8;
        } else {
            continue;
        }
        if (p >= n || s[p] != kPua202) continue;
        ++p;
        const std::size_t content = p;
        while (p < n && s[p] != kPua200 && s[p] != kPua201) ++p;
        if (p == content) continue;  // [^...]+ needs at least one character
        if (p < n && s[p] == kPua201) return Match{{i, p + 1}};
    }
    return std::nullopt;
}

/** \uE200(?:turn\d+[a-z]+\d+)(?:\uE202turn\d+[a-z]+\d+)*\uE201 */
Match find_bare_envelope(const U16& s, std::size_t from) {
    const std::size_t n = s.size();
    for (std::size_t i = from; i < n; ++i) {
        if (s[i] != kPua200) continue;
        std::size_t p = match_turn_group(s, i + 1);
        if (p == kNpos) continue;
        for (;;) {
            if (p < n && s[p] == kPua201) return Match{{i, p + 1}};
            if (p < n && s[p] == kPua202) {
                const std::size_t q = match_turn_group(s, p + 1);
                if (q != kNpos) {
                    p = q;
                    continue;
                }
            }
            break;
        }
    }
    return std::nullopt;
}

/** 【turn\d+[a-z]+\d+】 */
Match find_cjk_marker(const U16& s, std::size_t from) {
    const std::size_t n = s.size();
    for (std::size_t i = from; i < n; ++i) {
        if (s[i] != 0x3010) continue;
        const std::size_t p = match_turn_group(s, i + 1);
        if (p != kNpos && p < n && s[p] == 0x3011) return Match{{i, p + 1}};
    }
    return std::nullopt;
}

/** cite(?:turn\d+[a-z]+\d+)+ */
Match find_plain_artifact(const U16& s, std::size_t from) {
    const std::size_t n = s.size();
    for (std::size_t i = from; i < n; ++i) {
        if (!starts_with_fold(s, i, u"cite")) continue;
        std::size_t p = match_turn_group(s, i + 4);
        if (p == kNpos) continue;
        for (;;) {
            const std::size_t q = match_turn_group(s, p);
            if (q == kNpos) break;
            p = q;
        }
        return Match{{i, p}};
    }
    return std::nullopt;
}

/** Java's `$` without MULTILINE: end of input, or right before a final line terminator
 *  (a single \n, \r, NEL, LS, PS, or one \r\n pair). */
bool at_dollar(const U16& s, std::size_t pos) {
    const std::size_t n = s.size();
    if (pos == n) return true;
    if (pos + 2 == n && s[pos] == u'\r' && s[pos + 1] == u'\n') return true;
    if (pos + 1 == n) {
        const char16_t c = s[pos];
        return c == u'\n' || c == u'\r' || c == 0x0085 || c == 0x2028 || c == 0x2029;
    }
    return false;
}

/** The mandatory tail of the trailing pattern: turn(?:\d+(?:[a-z]+\d*)?)? followed by $.
 *  Returns where the tail match ends, or nullopt when the tail cannot match at all. */
std::optional<std::size_t> trailing_tail_end(const U16& s, std::size_t pos) {
    const std::size_t n = s.size();
    if (!starts_with_fold(s, pos, u"turn")) return std::nullopt;
    const std::size_t after_turn = pos + 4;
    if (at_dollar(s, after_turn)) return after_turn;  // the optional group is skipped
    std::size_t p = after_turn;
    const std::size_t digits1 = p;
    while (p < n && is_ascii_digit(s[p])) ++p;
    if (p == digits1) return std::nullopt;  // the optional group needs \d+
    if (at_dollar(s, p)) return p;  // \d+ without the letter part
    const std::size_t letters1 = p;
    while (p < n && is_ascii_alpha(s[p])) ++p;
    if (p == letters1) return std::nullopt;
    while (p < n && is_ascii_digit(s[p])) ++p;
    if (at_dollar(s, p)) return p;
    return std::nullopt;
}

/** cite(?:turn\d+[a-z]+\d+)*(?:turn(?:\d+(?:[a-z]+\d*)?)?)$ */
Match find_trailing_artifact(const U16& s, std::size_t from) {
    const std::size_t n = s.size();
    for (std::size_t i = from; i < n; ++i) {
        if (!starts_with_fold(s, i, u"cite")) continue;
        // Greedy group runs, then backtrack over the number of consumed groups.
        std::vector<std::size_t> ends;
        std::size_t p = i + 4;
        ends.push_back(p);
        for (;;) {
            const std::size_t q = match_turn_group(s, p);
            if (q == kNpos) break;
            ends.push_back(q);
            p = q;
        }
        for (std::size_t k = ends.size(); k-- > 0;) {
            const std::optional<std::size_t> tail = trailing_tail_end(s, ends[k]);
            if (tail) return Match{{i, *tail}};
        }
    }
    return std::nullopt;
}

/** Kotlin Regex.replace(input, ""): deletes every match found left to right. */
U16 remove_matches(const U16& input, Match (*finder)(const U16&, std::size_t)) {
    U16 out;
    std::size_t cursor = 0;
    std::size_t search_from = 0;
    for (;;) {
        const Match match = finder(input, search_from);
        if (!match) break;
        out.append(input, cursor, match->first - cursor);
        cursor = match->second;
        search_from = match->second;
    }
    out.append(input, cursor, input.size() - cursor);
    return out;
}

/** Kotlin String.replace(old, ""): deletes every occurrence of the literal. */
U16 remove_literal(const U16& input, const char16_t* old_word) {
    std::u16string_view needle(old_word);
    if (needle.empty()) return input;
    U16 out;
    std::size_t cursor = 0;
    for (;;) {
        const std::size_t hit = input.find(needle, cursor);
        if (hit == kNpos) break;
        out.append(input, cursor, hit - cursor);
        cursor = hit + needle.size();
    }
    out.append(input, cursor, input.size() - cursor);
    return out;
}

std::string strip_private_markers_impl(std::string_view text) {
    U16 current = utf16::from_utf8(text);
    current = remove_matches(current, find_private_envelope);
    current = remove_matches(current, find_bare_envelope);
    current = remove_matches(current, find_cjk_marker);
    current = remove_matches(current, find_plain_artifact);
    current = remove_matches(current, find_trailing_artifact);
    current = remove_literal(current, u"\uE200");
    current = remove_literal(current, u"\uE201");
    current = remove_literal(current, u"\uE202");
    return utf16::to_utf8(current);
}

// ---------------------------------------------------------------------------------------------
// CitationPolicy core
// ---------------------------------------------------------------------------------------------

std::optional<std::string> safe_http_url_impl(const std::optional<std::string>& raw) {
    if (!raw) return std::nullopt;
    const std::optional<std::string> value = bounded_plain(json::trim(*raw), policy::kMaxUrlChars);
    if (!value) return std::nullopt;
    // value.any(Char::isWhitespace)
    {
        const std::u16string units = utf16::from_utf8(*value);
        for (const char16_t unit : units) {
            if ((unicode::flags_of(unit) & (unicode::kJvmWhitespace | unicode::kJvmSpace)) != 0) {
                return std::nullopt;
            }
        }
    }
    const std::optional<java_uri::Uri> uri = java_uri::parse(*value);
    if (!uri) return std::nullopt;
    if (!uri->scheme) return std::nullopt;
    const std::string scheme = ascii_lower(*uri->scheme);
    if (scheme != "http" && scheme != "https") return std::nullopt;
    if (uri->user_info) return std::nullopt;
    if (!uri->host) return std::nullopt;
    const std::string host = ascii_lower(*uri->host);
    if (json::is_blank(host)) return std::nullopt;
    int normalized_port = uri->port;
    if (scheme == "http" && uri->port == 80) normalized_port = -1;
    else if (scheme == "https" && uri->port == 443) normalized_port = -1;
    const std::optional<java_uri::Uri> rebuilt =
        java_uri::build(scheme, std::nullopt, host, normalized_port, java_uri::decoded_path(*uri),
                        java_uri::decoded_query(*uri), java_uri::decoded_fragment(*uri));
    if (!rebuilt) return std::nullopt;
    const java_uri::Uri normalized = java_uri::normalize(*rebuilt);
    const std::string ascii = java_uri::to_ascii_string(normalized);
    if (json::utf16_length(ascii) > policy::kMaxUrlChars) return std::nullopt;
    return ascii;
}

std::optional<CitationAnchor> normalize_anchor(const CitationAnchor& anchor,
                                               const std::optional<std::string>& answer_text) {
    if (anchor.start_index < 0 || anchor.end_index <= anchor.start_index) return std::nullopt;
    const std::optional<std::string> supplied =
        bounded_plain(anchor.cited_text, policy::kMaxCitedTextChars);
    if (!supplied) return std::nullopt;
    if (!answer_text) {
        if (static_cast<std::int64_t>(anchor.end_index) - anchor.start_index !=
            static_cast<std::int64_t>(json::utf16_length(*supplied))) {
            return std::nullopt;
        }
        CitationAnchor copy = anchor;
        copy.cited_text = *supplied;
        return copy;
    }
    if (static_cast<std::int64_t>(anchor.end_index) >
        static_cast<std::int64_t>(json::utf16_length(*answer_text))) {
        return std::nullopt;
    }
    const std::u16string answer = utf16::from_utf8(*answer_text);
    const std::u16string actual = answer.substr(
        static_cast<std::size_t>(anchor.start_index),
        static_cast<std::size_t>(anchor.end_index - anchor.start_index));
    const std::u16string supplied_units = utf16::from_utf8(*supplied);
    if (actual == supplied_units) {
        CitationAnchor copy = anchor;
        copy.cited_text = utf16::to_utf8(actual);
        return copy;
    }
    const std::size_t recovered = utf16::index_of(answer, supplied_units, 0);
    if (recovered == kNpos) return std::nullopt;
    if (utf16::index_of(answer, supplied_units, recovered + 1) != kNpos) return std::nullopt;
    CitationAnchor recovered_anchor;
    recovered_anchor.start_index = static_cast<std::int32_t>(recovered);
    recovered_anchor.end_index = static_cast<std::int32_t>(recovered + supplied_units.size());
    recovered_anchor.cited_text = *supplied;
    return recovered_anchor;
}

std::optional<CitationRecord> create_impl(
    const std::string& provider, const std::string& kind,
    const std::optional<std::string>& title, const std::optional<std::string>& url,
    const std::optional<std::string>& file_name, const std::optional<std::string>& location,
    const std::optional<std::string>& provider_source_id,
    const std::optional<std::string>& excerpt, const std::vector<CitationAnchor>& anchors,
    const std::optional<std::string>& answer_text) {
    const std::optional<std::string> bounded_provider =
        bounded_plain(json::trim(provider), policy::kMaxMetadataChars);
    std::string normalized_provider = bounded_provider ? ascii_lower(*bounded_provider) : "";
    const std::optional<std::string> bounded_kind =
        bounded_plain(json::trim(kind), policy::kMaxMetadataChars);
    std::string normalized_kind = bounded_kind ? ascii_lower(*bounded_kind) : "";
    if (json::is_blank(normalized_provider) || json::is_blank(normalized_kind)) {
        return std::nullopt;
    }
    const std::optional<std::string> safe_url = safe_http_url_impl(url);
    const std::optional<std::string> normalized_file_name = bounded_metadata(file_name);
    const std::optional<std::string> normalized_private_id = bounded_metadata(provider_source_id);
    const std::optional<std::string> explicit_title = bounded_metadata(title);
    if (url && !json::is_blank(*url) && !safe_url && !explicit_title && !normalized_file_name &&
        !normalized_private_id) {
        return std::nullopt;
    }
    std::string normalized_title;
    if (explicit_title) {
        normalized_title = *explicit_title;
    } else if (normalized_file_name) {
        normalized_title = *normalized_file_name;
    } else if (safe_url) {
        normalized_title = url_host(*safe_url);
    } else {
        normalized_title = uppercase_first_char(normalized_provider);
    }
    if (!safe_url && !normalized_file_name && !normalized_private_id &&
        json::is_blank(normalized_title)) {
        return std::nullopt;
    }
    std::vector<CitationAnchor> normalized_anchors;
    for (const CitationAnchor& anchor : anchors) {
        const std::optional<CitationAnchor> normalized = normalize_anchor(anchor, answer_text);
        if (!normalized) continue;
        bool duplicate = false;
        for (const CitationAnchor& existing : normalized_anchors) {
            if (existing == *normalized) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) normalized_anchors.push_back(*normalized);
    }
    if (normalized_anchors.size() > policy::kMaxAnchorsPerSource) {
        normalized_anchors.resize(policy::kMaxAnchorsPerSource);
    }
    std::string identity;
    if (safe_url) {
        identity = "url:" + *safe_url;
    } else {
        identity = normalized_provider;
        identity.push_back('\x1f');
        identity += normalized_kind;
        identity.push_back('\x1f');
        identity += normalized_private_id ? *normalized_private_id : std::string();
        identity.push_back('\x1f');
        identity += normalized_file_name ? *normalized_file_name : std::string();
        identity.push_back('\x1f');
        identity += normalized_title;
    }
    CitationRecord record;
    record.version = policy::kVERSION;
    record.source_id = "citation_" + sha256_hex(identity).substr(0, 24);
    record.provider = std::move(normalized_provider);
    record.kind = std::move(normalized_kind);
    record.title = std::move(normalized_title);
    record.url = safe_url;
    record.file_name = normalized_file_name;
    record.location = bounded_metadata(location);
    record.provider_source_id = normalized_private_id;
    record.excerpt = bounded(excerpt, policy::kMaxExcerptChars);
    record.anchors = std::move(normalized_anchors);
    return record;
}

std::optional<CitationRecord> normalize_record_impl(
    const CitationRecord& record, const std::optional<std::string>& answer_text) {
    if (record.version != policy::kVERSION) return std::nullopt;
    return create_impl(record.provider, record.kind, record.title, record.url, record.file_name,
                       record.location, record.provider_source_id, record.excerpt, record.anchors,
                       answer_text);
}

CitationRecord merge_impl(const CitationRecord& first, const CitationRecord& incoming) {
    if (first.source_id != incoming.source_id) {
        throw std::invalid_argument("merge requires identical sourceIds");
    }
    std::vector<CitationAnchor> anchors = first.anchors;
    for (const CitationAnchor& anchor : incoming.anchors) {
        std::ptrdiff_t same_start = -1;
        for (std::size_t i = 0; i < anchors.size(); ++i) {
            if (anchors[i].start_index == anchor.start_index) {
                same_start = static_cast<std::ptrdiff_t>(i);
                break;
            }
        }
        bool contained = false;
        for (const CitationAnchor& existing : anchors) {
            if (existing == anchor) {
                contained = true;
                break;
            }
        }
        if (same_start >= 0 &&
            anchor.end_index >= anchors[static_cast<std::size_t>(same_start)].end_index) {
            anchors[static_cast<std::size_t>(same_start)] = anchor;
        } else if (same_start < 0 && !contained &&
                   anchors.size() < policy::kMaxAnchorsPerSource) {
            anchors.push_back(anchor);
        }
    }
    CitationRecord merged = first;
    if (json::is_blank(merged.title)) merged.title = incoming.title;
    if (!merged.url) merged.url = incoming.url;
    if (!merged.file_name) merged.file_name = incoming.file_name;
    if (!merged.location) merged.location = incoming.location;
    if (!merged.provider_source_id) merged.provider_source_id = incoming.provider_source_id;
    if (!merged.excerpt) merged.excerpt = incoming.excerpt;
    merged.anchors = std::move(anchors);
    return merged;
}

std::vector<CitationRecord> deduplicate_impl(const std::vector<CitationRecord>& records,
                                             const std::optional<std::string>& answer_text) {
    std::vector<CitationRecord> result;
    for (const CitationRecord& raw : records) {
        const std::optional<CitationRecord> citation = normalize_record_impl(raw, answer_text);
        if (!citation) continue;
        std::ptrdiff_t existing = -1;
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (result[i].source_id == citation->source_id) {
                existing = static_cast<std::ptrdiff_t>(i);
                break;
            }
        }
        if (existing < 0) {
            if (result.size() < policy::kMaxSources) result.push_back(*citation);
        } else {
            result[static_cast<std::size_t>(existing)] =
                merge_impl(result[static_cast<std::size_t>(existing)], *citation);
        }
    }
    return result;
}

std::string copy_text_impl(std::string_view answer, const std::vector<CitationRecord>& records) {
    const std::string cleaned = strip_private_markers_impl(answer);
    const std::vector<CitationRecord> normalized = deduplicate_impl(records, cleaned);
    if (normalized.empty()) return cleaned;
    std::string out = trim_end(cleaned);
    out += "\n\n## Sources\n";
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const CitationRecord& citation = normalized[i];
        const std::string title = escape_markdown_label(citation.title);
        std::string location_suffix;
        if (citation.location) location_suffix = " - " + *citation.location;
        std::string rendered;
        if (citation.url) {
            std::string replaced;
            replaced.reserve(citation.url->size());
            for (const char c : *citation.url) {
                if (c == ')') {
                    replaced += "%29";
                } else {
                    replaced.push_back(c);
                }
            }
            rendered = "[" + title + "](" + replaced + ")";
        } else {
            rendered = title;
        }
        if (i != 0) out += '\n';
        out += std::to_string(i + 1);
        out += ". ";
        out += rendered;
        out += location_suffix;
    }
    return out;
}

void encode_anchor(std::string& out, const CitationAnchor& anchor) {
    out += "{\"startIndex\":";
    out += std::to_string(anchor.start_index);
    out += ",\"endIndex\":";
    out += std::to_string(anchor.end_index);
    out += ",\"citedText\":\"";
    json::escape_to(out, anchor.cited_text);
    out += "\"}";
}

std::string encode_impl(const CitationRecord& record) {
    std::string out = "{";
    bool first = true;
    const auto field = [&](std::string_view name) {
        if (!first) out += ',';
        first = false;
        out += '"';
        out += name;
        out += "\":";
    };
    if (record.version != policy::kVERSION) {
        field("version");
        out += std::to_string(record.version);
    }
    field("sourceId");
    out += '"';
    json::escape_to(out, record.source_id);
    out += '"';
    field("provider");
    out += '"';
    json::escape_to(out, record.provider);
    out += '"';
    field("kind");
    out += '"';
    json::escape_to(out, record.kind);
    out += '"';
    field("title");
    out += '"';
    json::escape_to(out, record.title);
    out += '"';
    if (record.url) {
        field("url");
        out += '"';
        json::escape_to(out, *record.url);
        out += '"';
    }
    if (record.file_name) {
        field("fileName");
        out += '"';
        json::escape_to(out, *record.file_name);
        out += '"';
    }
    if (record.location) {
        field("location");
        out += '"';
        json::escape_to(out, *record.location);
        out += '"';
    }
    if (record.provider_source_id) {
        field("providerSourceId");
        out += '"';
        json::escape_to(out, *record.provider_source_id);
        out += '"';
    }
    if (record.excerpt) {
        field("excerpt");
        out += '"';
        json::escape_to(out, *record.excerpt);
        out += '"';
    }
    if (!record.anchors.empty()) {
        field("anchors");
        out += '[';
        for (std::size_t i = 0; i < record.anchors.size(); ++i) {
            if (i != 0) out += ',';
            encode_anchor(out, record.anchors[i]);
        }
        out += ']';
    }
    out += '}';
    return out;
}

// ---------------------------------------------------------------------------------------------
// CitationPolicy.decode: kotlinx decodeFromString<CitationRecord> with ignoreUnknownKeys=true.
// The numeric machinery is a verbatim copy of the openai stream port, which was pinned against
// AbstractJsonLexer.consumeNumericLiteral by its own goldens.
// ---------------------------------------------------------------------------------------------

std::int64_t wrap_mul10_sub(std::int64_t value, int digit) {
    const std::uint64_t wrapped =
        static_cast<std::uint64_t>(value) * 10u - static_cast<std::uint64_t>(digit);
    return static_cast<std::int64_t>(wrapped);
}

std::int64_t wrap_mul10_add(std::int64_t value, int digit) {
    const std::uint64_t wrapped =
        static_cast<std::uint64_t>(value) * 10u + static_cast<std::uint64_t>(digit);
    return static_cast<std::int64_t>(wrapped);
}

double pow10_like_jvm(double exponent) {
    if (exponent >= -400.0 && exponent <= 400.0 && std::floor(exponent) == exponent) {
        const auto index = static_cast<int>(exponent) + 400;
        double result = 0.0;
        std::memcpy(&result, &openai::kJvmPow10Bits[index], sizeof(result));
        return result;
    }
    return std::pow(10.0, exponent);
}

double calculate_exponent(std::int64_t exponent_accumulator, bool is_exponent_positive) {
    const double value = static_cast<double>(exponent_accumulator);
    return is_exponent_positive ? pow10_like_jvm(value) : pow10_like_jvm(-value);
}

bool decode_numeric_literal(std::string_view source, std::size_t start, std::int64_t& out) {
    std::size_t current = start;
    while (current < source.size()) {
        const char c = source[current];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            ++current;
        } else {
            break;
        }
    }
    if (current >= source.size()) return false;
    bool has_quotation = false;
    if (source[current] == '"') {
        ++current;
        if (current >= source.size()) return false;
        has_quotation = true;
    }
    std::int64_t accumulator = 0;
    std::int64_t exponent_accumulator = 0;
    bool is_negative = false;
    bool is_exponent_positive = false;
    bool has_exponent = false;
    const std::size_t literal_start = current;
    while (current < source.size()) {
        const char ch = source[current];
        if ((ch == 'e' || ch == 'E') && !has_exponent) {
            if (current == literal_start) return false;
            is_exponent_positive = true;
            has_exponent = true;
            ++current;
            continue;
        }
        if (ch == '-' && has_exponent) {
            if (current == literal_start) return false;
            is_exponent_positive = false;
            ++current;
            continue;
        }
        if (ch == '+' && has_exponent) {
            if (current == literal_start) return false;
            is_exponent_positive = true;
            ++current;
            continue;
        }
        if (ch == '-') {
            if (current != literal_start) return false;
            is_negative = true;
            ++current;
            continue;
        }
        if (!json::is_other_token_byte(static_cast<unsigned char>(ch))) break;
        ++current;
        const int digit = static_cast<unsigned char>(ch) - '0';
        if (digit < 0 || digit > 9) return false;
        if (has_exponent) {
            exponent_accumulator = wrap_mul10_add(exponent_accumulator, digit);
        } else {
            accumulator = wrap_mul10_sub(accumulator, digit);
            if (accumulator > 0) return false;
        }
    }
    const bool has_chars = current != literal_start;
    if (literal_start == current || (is_negative && literal_start == current - 1)) return false;
    if (has_quotation) {
        if (!has_chars) return false;
        if (current >= source.size() || source[current] != '"') return false;
        ++current;
    }
    if (has_exponent) {
        double double_accumulator = static_cast<double>(accumulator) *
                                    calculate_exponent(exponent_accumulator, is_exponent_positive);
        if (double_accumulator > 9223372036854775807.0) return false;
        if (double_accumulator < -9223372036854775808.0) return false;
        if (std::floor(double_accumulator) != double_accumulator) return false;
        accumulator = static_cast<std::int64_t>(double_accumulator);
    }
    if (is_negative) {
        out = accumulator;
        return true;
    }
    if (accumulator == INT64_MIN) return false;
    out = -accumulator;
    return true;
}

/** An Int property: any token goes through consumeNumericLiteral, so quoted numbers pass and
 *  every other token (including null, floats and string escapes) fails. */
bool decode_int_token(std::string_view source, const json::Value& value, std::int32_t& out) {
    std::int64_t parsed = 0;
    if (!decode_numeric_literal(source, value.begin, parsed)) return false;
    if (parsed != static_cast<std::int64_t>(static_cast<std::int32_t>(parsed))) return false;
    out = static_cast<std::int32_t>(parsed);
    return true;
}

/** CitationPolicy.decode before normalize: a strict walk over the parsed document. Every
 *  rejection maps to null, exactly like runCatching { decodeFromString(...) }.getOrNull().
 *  Unknown keys are skipped (ignoreUnknownKeys) and duplicates overwrite (last wins). */
std::optional<CitationRecord> decode_record(std::string_view raw, const json::Value& root) {
    if (root.kind != json::Kind::Object) return std::nullopt;
    std::int32_t version = policy::kVERSION;
    std::optional<std::string> source_id;
    std::optional<std::string> provider;
    std::optional<std::string> kind;
    std::optional<std::string> title;
    std::optional<std::string> url;
    std::optional<std::string> file_name;
    std::optional<std::string> location;
    std::optional<std::string> provider_source_id;
    std::optional<std::string> excerpt;
    std::vector<CitationAnchor> anchors;
    for (const json::Member& member : root.members) {
        const json::Value& value = member.value;
        if (member.key == "version") {
            if (!decode_int_token(raw, value, version)) return std::nullopt;
        } else if (member.key == "sourceId") {
            if (value.kind != json::Kind::String) return std::nullopt;
            source_id = value.text;
        } else if (member.key == "provider") {
            if (value.kind != json::Kind::String) return std::nullopt;
            provider = value.text;
        } else if (member.key == "kind") {
            if (value.kind != json::Kind::String) return std::nullopt;
            kind = value.text;
        } else if (member.key == "title") {
            if (value.kind != json::Kind::String) return std::nullopt;
            title = value.text;
        } else if (member.key == "url") {
            if (value.is_literal("null")) {
                url = std::nullopt;
            } else if (value.kind == json::Kind::String) {
                url = value.text;
            } else {
                return std::nullopt;
            }
        } else if (member.key == "fileName") {
            if (value.is_literal("null")) {
                file_name = std::nullopt;
            } else if (value.kind == json::Kind::String) {
                file_name = value.text;
            } else {
                return std::nullopt;
            }
        } else if (member.key == "location") {
            if (value.is_literal("null")) {
                location = std::nullopt;
            } else if (value.kind == json::Kind::String) {
                location = value.text;
            } else {
                return std::nullopt;
            }
        } else if (member.key == "providerSourceId") {
            if (value.is_literal("null")) {
                provider_source_id = std::nullopt;
            } else if (value.kind == json::Kind::String) {
                provider_source_id = value.text;
            } else {
                return std::nullopt;
            }
        } else if (member.key == "excerpt") {
            if (value.is_literal("null")) {
                excerpt = std::nullopt;
            } else if (value.kind == json::Kind::String) {
                excerpt = value.text;
            } else {
                return std::nullopt;
            }
        } else if (member.key == "anchors") {
            if (value.kind != json::Kind::Array) return std::nullopt;
            std::vector<CitationAnchor> parsed_anchors;
            for (const json::Value& item : value.elements) {
                if (item.kind != json::Kind::Object) return std::nullopt;
                std::optional<std::int32_t> start_index;
                std::optional<std::int32_t> end_index;
                std::optional<std::string> cited_text;
                for (const json::Member& anchor_member : item.members) {
                    if (anchor_member.key == "startIndex") {
                        std::int32_t parsed = 0;
                        if (!decode_int_token(raw, anchor_member.value, parsed)) return std::nullopt;
                        start_index = parsed;
                    } else if (anchor_member.key == "endIndex") {
                        std::int32_t parsed = 0;
                        if (!decode_int_token(raw, anchor_member.value, parsed)) return std::nullopt;
                        end_index = parsed;
                    } else if (anchor_member.key == "citedText") {
                        if (anchor_member.value.kind != json::Kind::String) return std::nullopt;
                        cited_text = anchor_member.value.text;
                    }
                }
                if (!start_index || !end_index || !cited_text) return std::nullopt;
                CitationAnchor anchor;
                anchor.start_index = *start_index;
                anchor.end_index = *end_index;
                anchor.cited_text = *cited_text;
                parsed_anchors.push_back(std::move(anchor));
            }
            anchors = std::move(parsed_anchors);
        }
    }
    if (!source_id || !provider || !kind || !title) return std::nullopt;
    CitationRecord record;
    record.version = version;
    record.source_id = *source_id;
    record.provider = *provider;
    record.kind = *kind;
    record.title = *title;
    record.url = url;
    record.file_name = file_name;
    record.location = location;
    record.provider_source_id = provider_source_id;
    record.excerpt = excerpt;
    record.anchors = std::move(anchors);
    return record;
}

std::optional<CitationRecord> decode_impl(std::string_view raw,
                                          const std::optional<std::string>& answer_text) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value) return std::nullopt;
    const std::optional<CitationRecord> record = decode_record(raw, *parsed.value);
    if (!record) return std::nullopt;
    return normalize_record_impl(*record, answer_text);
}

}  // namespace

}  // namespace agora::core::citations

// =============================================================================================
// Public surface
// =============================================================================================

namespace agora::core::citations {

bool operator==(const CitationAnchor& first, const CitationAnchor& second) {
    return first.start_index == second.start_index && first.end_index == second.end_index &&
           first.cited_text == second.cited_text;
}

bool operator!=(const CitationAnchor& first, const CitationAnchor& second) {
    return !(first == second);
}

bool operator==(const CitationRecord& first, const CitationRecord& second) {
    return first.version == second.version && first.source_id == second.source_id &&
           first.provider == second.provider && first.kind == second.kind &&
           first.title == second.title && first.url == second.url &&
           first.file_name == second.file_name && first.location == second.location &&
           first.provider_source_id == second.provider_source_id &&
           first.excerpt == second.excerpt && first.anchors == second.anchors;
}

bool operator!=(const CitationRecord& first, const CitationRecord& second) {
    return !(first == second);
}

namespace policy {

std::optional<CitationRecord> create(
    const std::string& provider, const std::string& kind, const std::optional<std::string>& title,
    const std::optional<std::string>& url, const std::optional<std::string>& file_name,
    const std::optional<std::string>& location,
    const std::optional<std::string>& provider_source_id,
    const std::optional<std::string>& excerpt, const std::vector<CitationAnchor>& anchors,
    const std::optional<std::string>& answer_text) {
    return create_impl(provider, kind, title, url, file_name, location, provider_source_id, excerpt,
                       anchors, answer_text);
}

std::optional<CitationRecord> normalize_record(const CitationRecord& record,
                                               const std::optional<std::string>& answer_text) {
    return normalize_record_impl(record, answer_text);
}

CitationRecord merge(const CitationRecord& first, const CitationRecord& incoming) {
    return merge_impl(first, incoming);
}

std::string encode(const CitationRecord& record) { return encode_impl(record); }

std::optional<CitationRecord> decode(std::string_view raw,
                                     const std::optional<std::string>& answer_text) {
    return decode_impl(raw, answer_text);
}

std::optional<std::string> safe_http_url(const std::optional<std::string>& raw) {
    return safe_http_url_impl(raw);
}

std::string strip_private_markers(std::string_view text) {
    return strip_private_markers_impl(text);
}

std::string copy_text(std::string_view answer, const std::vector<CitationRecord>& records) {
    return copy_text_impl(answer, records);
}

std::vector<CitationRecord> deduplicate(const std::vector<CitationRecord>& records,
                                        const std::optional<std::string>& answer_text) {
    return deduplicate_impl(records, answer_text);
}

}  // namespace policy

// =============================================================================================
// Projections (kotlinx JsonElement tree decode against the collapsed object)
// =============================================================================================

namespace {

/** kotlinx JsonObject parsing collapses duplicate keys with the last value winning, so every
 *  tree lookup here keeps the last member with the requested key. */
const json::Value* find_member(const json::Value& object, std::string_view key) {
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

/** JsonPrimitive.content: strings contribute their decoded text, other tokens their raw text
 *  (JsonNull becomes "null" through this path, exactly like the reference). Objects and
 *  arrays are not primitives. */
std::optional<std::string> primitive_content(const json::Value& value) {
    if (value.kind == json::Kind::String || value.kind == json::Kind::Literal) return value.text;
    return std::nullopt;
}

/** Kotlin String.toIntOrNull: optional sign, ASCII digits, Int range. (Unicode digits are a
 *  documented boundary; see the header.) */
std::optional<std::int32_t> to_int_or_null(const std::string& text) {
    const std::u16string units = utf16::from_utf8(text);
    if (units.empty()) return std::nullopt;
    std::size_t i = 0;
    bool negative = false;
    if (units[i] == u'+' || units[i] == u'-') {
        negative = units[i] == u'-';
        ++i;
        if (i >= units.size()) return std::nullopt;
    }
    std::int64_t value = 0;
    for (; i < units.size(); ++i) {
        if (!is_ascii_digit(units[i])) return std::nullopt;
        value = value * 10 + (units[i] - u'0');
        if (value > 2147483648LL) return std::nullopt;
    }
    if (negative) value = -value;
    if (value < -2147483648LL || value > 2147483647LL) return std::nullopt;
    return static_cast<std::int32_t>(value);
}

/** Tree decode of a nullable String property: absent or JsonNull means null, a non-string
 *  primitive is a decode error (ok = false). */
struct TreeString {
    bool ok = true;
    std::optional<std::string> value;
};

TreeString tree_string(const json::Value* member) {
    TreeString out;
    if (member == nullptr) return out;
    if (member->is_literal("null")) return out;
    if (member->kind != json::Kind::String) {
        out.ok = false;
        return out;
    }
    out.value = member->text;
    return out;
}

/** Tree decode of a nullable Int property: JsonNull means null, any primitive goes through
 *  content.toIntOrNull() (so quoted numbers pass and 1.5 fails). */
struct TreeInt {
    bool ok = true;
    std::optional<std::int32_t> value;
};

TreeInt tree_int(const json::Value* member) {
    TreeInt out;
    if (member == nullptr) return out;
    if (member->is_literal("null")) return out;
    const std::optional<std::string> content = member ? primitive_content(*member) : std::nullopt;
    if (!content) {
        out.ok = false;
        return out;
    }
    out.value = to_int_or_null(*content);
    if (!out.value) out.ok = false;
    return out;
}

/** The private @Serializable AnthropicCitation, field for field. */
struct AnthropicCitation {
    std::optional<std::string> type;
    std::optional<std::string> cited_text;
    std::optional<std::int32_t> document_index;
    std::optional<std::string> document_title;
    std::optional<std::int32_t> start_char_index;
    std::optional<std::int32_t> end_char_index;
    std::optional<std::int32_t> start_page_number;
    std::optional<std::int32_t> end_page_number;
    std::optional<std::int32_t> start_block_index;
    std::optional<std::int32_t> end_block_index;
    std::optional<std::string> encrypted_index;
    std::optional<std::int32_t> search_result_index;
    std::optional<std::string> title;
    std::optional<std::string> url;
    std::optional<std::string> source;
};

std::optional<AnthropicCitation> decode_anthropic_citation(const json::Value& element) {
    if (element.kind != json::Kind::Object) return std::nullopt;
    AnthropicCitation citation;
    const TreeString type = tree_string(find_member(element, "type"));
    if (!type.ok) return std::nullopt;
    citation.type = type.value;
    const TreeString cited_text = tree_string(find_member(element, "cited_text"));
    if (!cited_text.ok) return std::nullopt;
    citation.cited_text = cited_text.value;
    const TreeInt document_index = tree_int(find_member(element, "document_index"));
    if (!document_index.ok) return std::nullopt;
    citation.document_index = document_index.value;
    const TreeString document_title = tree_string(find_member(element, "document_title"));
    if (!document_title.ok) return std::nullopt;
    citation.document_title = document_title.value;
    const TreeInt start_char_index = tree_int(find_member(element, "start_char_index"));
    if (!start_char_index.ok) return std::nullopt;
    citation.start_char_index = start_char_index.value;
    const TreeInt end_char_index = tree_int(find_member(element, "end_char_index"));
    if (!end_char_index.ok) return std::nullopt;
    citation.end_char_index = end_char_index.value;
    const TreeInt start_page_number = tree_int(find_member(element, "start_page_number"));
    if (!start_page_number.ok) return std::nullopt;
    citation.start_page_number = start_page_number.value;
    const TreeInt end_page_number = tree_int(find_member(element, "end_page_number"));
    if (!end_page_number.ok) return std::nullopt;
    citation.end_page_number = end_page_number.value;
    const TreeInt start_block_index = tree_int(find_member(element, "start_block_index"));
    if (!start_block_index.ok) return std::nullopt;
    citation.start_block_index = start_block_index.value;
    const TreeInt end_block_index = tree_int(find_member(element, "end_block_index"));
    if (!end_block_index.ok) return std::nullopt;
    citation.end_block_index = end_block_index.value;
    const TreeString encrypted_index = tree_string(find_member(element, "encrypted_index"));
    if (!encrypted_index.ok) return std::nullopt;
    citation.encrypted_index = encrypted_index.value;
    const TreeInt search_result_index = tree_int(find_member(element, "search_result_index"));
    if (!search_result_index.ok) return std::nullopt;
    citation.search_result_index = search_result_index.value;
    const TreeString title = tree_string(find_member(element, "title"));
    if (!title.ok) return std::nullopt;
    citation.title = title.value;
    const TreeString url = tree_string(find_member(element, "url"));
    if (!url.ok) return std::nullopt;
    citation.url = url.value;
    const TreeString source = tree_string(find_member(element, "source"));
    if (!source.ok) return std::nullopt;
    citation.source = source.value;
    return citation;
}

std::optional<std::string> range_location(const char* label,
                                          const std::optional<std::int32_t>& start,
                                          const std::optional<std::int32_t>& end) {
    if (start && end) {
        return std::string(label) + " " + std::to_string(*start) + "-" + std::to_string(*end);
    }
    if (start) return std::string(label) + " " + std::to_string(*start);
    if (end) return std::string(label) + " " + std::to_string(*end);
    return std::nullopt;
}

std::optional<std::string> search_result_source_id(const AnthropicCitation& citation) {
    std::vector<std::string> parts;
    if (citation.source && !json::is_blank(*citation.source)) parts.push_back(*citation.source);
    if (citation.search_result_index) {
        parts.push_back("result:" + std::to_string(*citation.search_result_index));
    }
    if (parts.empty()) return std::nullopt;
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) joined += '|';
        joined += parts[i];
    }
    return joined;
}

std::optional<CitationRecord> to_document_citation(const AnthropicCitation& citation,
                                                   const std::optional<std::string>& location,
                                                   const std::vector<CitationAnchor>& anchors) {
    std::optional<std::string> provider_source_id;
    if (citation.document_index) {
        provider_source_id = "document:" + std::to_string(*citation.document_index);
    }
    return create_impl("anthropic", "document", citation.document_title, std::nullopt, std::nullopt,
                       location, provider_source_id, citation.cited_text, anchors, std::nullopt);
}

// --- Gemini helpers --------------------------------------------------------------------------

const json::Value* array_content(const json::Value& object, std::string_view first_key,
                                 std::string_view second_key) {
    const json::Value* first = find_member(object, first_key);
    if (first && first->kind == json::Kind::Array) return first;
    const json::Value* second = find_member(object, second_key);
    if (second && second->kind == json::Kind::Array) return second;
    return nullptr;
}

const json::Value* object_content(const json::Value& object, std::string_view key) {
    const json::Value* value = find_member(object, key);
    if (value && value->kind == json::Kind::Object) return value;
    return nullptr;
}

/** firstNotNullOfOrNull over the two key spellings: a key whose primitive content is blank is
 *  skipped, and so is a key whose value is not a primitive. */
std::optional<std::string> string_content(const json::Value& object, std::string_view key) {
    const json::Value* value = find_member(object, key);
    if (value == nullptr) return std::nullopt;
    const std::optional<std::string> content = primitive_content(*value);
    if (!content) return std::nullopt;
    if (json::is_blank(*content)) return std::nullopt;
    return content;
}

std::optional<std::int32_t> int_content(const json::Value& object, std::string_view first_key,
                                        std::string_view second_key) {
    const std::string_view keys[2] = {first_key, second_key};
    for (const std::string_view key : keys) {
        const json::Value* value = find_member(object, key);
        if (value == nullptr) continue;
        const std::optional<std::string> content = primitive_content(*value);
        if (!content) continue;
        const std::optional<std::int32_t> parsed = to_int_or_null(*content);
        if (parsed) return parsed;
    }
    return std::nullopt;
}

/** The UTF-8 byte index to UTF-16 char index map of utf8BoundaryMap. Returns nullopt for
 *  text with lone surrogates (unreachable for the UTF-8 inputs of this port). */
std::optional<std::vector<std::int32_t>> utf8_boundary_map(const std::string& text) {
    const std::u16string units = utf16::from_utf8(text);
    std::vector<std::int32_t> boundaries(text.size() + 1, -1);
    std::size_t char_index = 0;
    std::size_t byte_index = 0;
    boundaries[0] = 0;
    while (char_index < units.size()) {
        const char16_t first = units[char_index];
        std::uint32_t code_point = 0;
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (char_index + 1 >= units.size()) return std::nullopt;
            const char16_t second = units[char_index + 1];
            if (second < 0xDC00 || second > 0xDFFF) return std::nullopt;
            code_point = 0x10000 + ((static_cast<std::uint32_t>(first) - 0xD800) << 10) +
                         (static_cast<std::uint32_t>(second) - 0xDC00);
            char_index += 2;
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
            return std::nullopt;
        } else {
            code_point = first;
            char_index += 1;
        }
        byte_index += code_point <= 0x7F ? 1 : code_point <= 0x7FF ? 2 : code_point <= 0xFFFF ? 3 : 4;
        if (byte_index >= boundaries.size()) return std::nullopt;
        boundaries[byte_index] = static_cast<std::int32_t>(char_index);
    }
    if (byte_index != boundaries.size() - 1) return std::nullopt;
    return boundaries;
}

std::optional<std::pair<std::int32_t, std::int32_t>> utf8_byte_range_to_utf16(
    const std::string& text, std::int32_t start_byte, std::int32_t end_byte) {
    if (start_byte < 0 || end_byte <= start_byte) return std::nullopt;
    const std::optional<std::vector<std::int32_t>> boundaries = utf8_boundary_map(text);
    if (!boundaries) return std::nullopt;
    if (end_byte >= static_cast<std::int32_t>(boundaries->size())) return std::nullopt;
    const std::int32_t start_index = (*boundaries)[static_cast<std::size_t>(start_byte)];
    const std::int32_t end_index = (*boundaries)[static_cast<std::size_t>(end_byte)];
    if (start_index < 0 || end_index <= start_index) return std::nullopt;
    return std::make_pair(start_index, end_index);
}

std::optional<CitationAnchor> to_gemini_anchor(const json::Value& segment,
                                               const std::string& answer_text) {
    const std::optional<std::int32_t> start_content = int_content(segment, "startIndex", "start_index");
    const std::int32_t start_byte = start_content ? *start_content : 0;
    const std::optional<std::int32_t> end_byte = int_content(segment, "endIndex", "end_index");
    if (!end_byte) return std::nullopt;
    const std::optional<std::pair<std::int32_t, std::int32_t>> range =
        utf8_byte_range_to_utf16(answer_text, start_byte, *end_byte);
    if (!range) return std::nullopt;
    const std::u16string units = utf16::from_utf8(answer_text);
    const std::u16string cited =
        units.substr(static_cast<std::size_t>(range->first),
                     static_cast<std::size_t>(range->second - range->first));
    const std::optional<std::string> supplied = string_content(segment, "text");
    if (supplied && *supplied != utf16::to_utf8(cited)) return std::nullopt;
    CitationAnchor anchor;
    anchor.start_index = range->first;
    anchor.end_index = range->second;
    anchor.cited_text = utf16::to_utf8(cited);
    return anchor;
}

}  // namespace

std::optional<CitationRecord> to_anthropic_citation_record(const json::Value& element,
                                                           std::int32_t answer_start_index,
                                                           const std::string& answer_text) {
    const std::optional<AnthropicCitation> decoded = decode_anthropic_citation(element);
    if (!decoded) return std::nullopt;
    const AnthropicCitation& citation = *decoded;
    std::vector<CitationAnchor> anchors;
    if (!answer_text.empty()) {
        CitationAnchor anchor;
        anchor.start_index = answer_start_index;
        anchor.end_index = static_cast<std::int32_t>(
            static_cast<std::int64_t>(answer_start_index) +
            static_cast<std::int64_t>(json::utf16_length(answer_text)));
        anchor.cited_text = answer_text;
        anchors.push_back(std::move(anchor));
    }
    if (!citation.type) return std::nullopt;
    const std::string type = ascii_lower(*citation.type);
    if (type == "web_search_result_location") {
        return create_impl("anthropic", "url", citation.title, citation.url, std::nullopt,
                           std::nullopt, citation.encrypted_index, citation.cited_text, anchors,
                           std::nullopt);
    }
    if (type == "search_result_location") {
        const std::string kind = safe_http_url_impl(citation.source) ? "url" : "document";
        return create_impl("anthropic", kind, citation.title, citation.source, std::nullopt,
                           range_location("Blocks", citation.start_block_index,
                                          citation.end_block_index),
                           search_result_source_id(citation), citation.cited_text, anchors,
                           std::nullopt);
    }
    if (type == "char_location") {
        return to_document_citation(
            citation,
            range_location("Characters", citation.start_char_index, citation.end_char_index),
            anchors);
    }
    if (type == "page_location") {
        return to_document_citation(
            citation, range_location("Pages", citation.start_page_number, citation.end_page_number),
            anchors);
    }
    if (type == "content_block_location") {
        return to_document_citation(
            citation,
            range_location("Blocks", citation.start_block_index, citation.end_block_index),
            anchors);
    }
    return std::nullopt;
}

std::vector<CitationRecord> to_gemini_citations(const json::Value& element,
                                                const std::string& answer_text) {
    std::vector<CitationRecord> citations;
    if (element.kind != json::Kind::Object) return citations;
    const json::Value* chunks = array_content(element, "groundingChunks", "grounding_chunks");
    if (chunks == nullptr) return citations;
    const json::Value* supports = array_content(element, "groundingSupports", "grounding_supports");
    if (supports == nullptr) return citations;
    for (const json::Value& support_element : supports->elements) {
        if (support_element.kind != json::Kind::Object) continue;
        const json::Value* segment = object_content(support_element, "segment");
        const std::optional<CitationAnchor> anchor =
            segment ? to_gemini_anchor(*segment, answer_text) : std::nullopt;
        const json::Value* indices =
            array_content(support_element, "groundingChunkIndices", "grounding_chunk_indices");
        std::vector<std::int32_t> chunk_indices;
        if (indices != nullptr) {
            for (const json::Value& item : indices->elements) {
                const std::optional<std::string> content = primitive_content(item);
                if (!content) continue;
                const std::optional<std::int32_t> value = to_int_or_null(*content);
                if (!value) continue;
                bool duplicate = false;
                for (const std::int32_t existing : chunk_indices) {
                    if (existing == *value) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) chunk_indices.push_back(*value);
            }
        }
        for (const std::int32_t chunk_index : chunk_indices) {
            if (chunk_index < 0 ||
                static_cast<std::size_t>(chunk_index) >= chunks->elements.size()) {
                continue;
            }
            const json::Value& chunk = chunks->elements[static_cast<std::size_t>(chunk_index)];
            if (chunk.kind != json::Kind::Object) continue;
            const json::Value* web = object_content(chunk, "web");
            if (web == nullptr) continue;
            const std::optional<std::string> title = string_content(*web, "title");
            const std::optional<std::string> uri = string_content(*web, "uri");
            std::vector<CitationAnchor> anchors;
            if (anchor) anchors.push_back(*anchor);
            std::optional<CitationRecord> record =
                create_impl("gemini", "url", title, uri, std::nullopt, std::nullopt, std::nullopt,
                            std::nullopt, anchors, answer_text);
            if (record) citations.push_back(std::move(*record));
        }
    }
    return deduplicate_impl(citations, answer_text);
}

bool matches_citation_title(const std::vector<CitationRecord>& records, const std::string& query) {
    if (json::is_blank(query)) return false;
    for (const CitationRecord& record : records) {
        if (contains_ignore_case(record.title, query)) return true;
    }
    return false;
}

}  // namespace agora::core::citations
