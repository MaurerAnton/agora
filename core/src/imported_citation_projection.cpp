// Port of data/ImportedCitationProjection.kt.
//
// The file is a pure transform over the export's raw answer text and reference JSON, so the port
// mirrors it literally: the two private marker regexes are scanned over UTF-16 (Kotlin String
// indexes), reference walking reproduces the firstNotNullOfOrNull / putIfAbsent order, and every
// anchor decision goes through the already-pinned CitationPolicy and MessagePersistenceGuard.
#include "agora/core/imported_citation_projection.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/backup_media_policy.hpp"
#include "agora/core/message_persistence_guard.hpp"
#include "utf16.hpp"

namespace agora::core::imported_citation {
namespace {

namespace json = agora::core::json;
using json::Kind;
using json::Value;
using U16 = std::u16string;
namespace policy = agora::core::citations::policy;

constexpr char16_t kPua200 = 0xE200;
constexpr char16_t kPua201 = 0xE201;
constexpr char16_t kPua202 = 0xE202;
constexpr char16_t kCjkOpen = 0x3010;   // 【
constexpr char16_t kCjkClose = 0x3011;  // 】

constexpr std::size_t kNpos = std::string::npos;

// ------------------------------------------------------------------------------- int accessor

/**
 * kotlinx JsonPrimitive.intOrNull — the JSON-literal grammar the reference accessors use:
 * Java-style trim (bytes <= U+0020), optional '-', ASCII digits with an optional decimal
 * exponent, Int range. Same rules as the `parse_int_literal` pinned by the
 * portable-settings-archive stand (and by scenarios 27/28/29 of this stand, including "3e2" -> 300
 * and "8.0" -> null).
 */
std::optional<int> int_or_null(std::string_view raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && static_cast<unsigned char>(raw[begin]) <= 0x20) ++begin;
    while (end > begin && static_cast<unsigned char>(raw[end - 1]) <= 0x20) --end;
    const std::string text(raw.substr(begin, end - begin));
    if (text.empty()) return std::nullopt;

    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '-') {
        negative = true;
        ++pos;
    }
    const std::size_t digits_begin = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    if (pos == digits_begin) return std::nullopt;

    long long exponent = 0;
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        bool exponent_negative = false;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            exponent_negative = text[pos] == '-';
            ++pos;
        }
        const std::size_t exponent_begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (exponent < 100000) exponent = exponent * 10 + (text[pos] - '0');
            ++pos;
        }
        if (pos != text.size()) return std::nullopt;
        if (pos == exponent_begin) exponent = 0;
        if (exponent_negative) exponent = -exponent;
    }
    if (pos != text.size()) return std::nullopt;

    const std::size_t mantissa_end = text.find_first_of("eE", digits_begin);
    std::string digits = text.substr(digits_begin, mantissa_end == std::string::npos
                                                       ? std::string::npos
                                                       : mantissa_end - digits_begin);
    const std::size_t first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) return 0;
    digits.erase(0, first_nonzero);

    if (exponent < 0) {
        const auto drop = static_cast<std::size_t>(-exponent);
        if (digits.size() <= drop) return std::nullopt;
        for (std::size_t i = digits.size() - drop; i < digits.size(); ++i) {
            if (digits[i] != '0') return std::nullopt;
        }
        digits.resize(digits.size() - drop);
    } else {
        if (exponent > 20) return std::nullopt;
        digits.append(static_cast<std::size_t>(exponent), '0');
    }

    if (digits.size() > 10) return std::nullopt;
    long long magnitude = 0;
    for (char digit : digits) magnitude = magnitude * 10 + (digit - '0');
    if (negative) magnitude = -magnitude;
    if (magnitude < std::numeric_limits<int>::min() ||
        magnitude > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(magnitude);
}

// ------------------------------------------------------------------------------ UTF-16 string ops

bool fold_ascii(char16_t a, char16_t b) {
    const auto lower = [](char16_t c) {
        return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c - u'A' + u'a') : c;
    };
    return lower(a) == lower(b);
}

bool starts_with_fold(const U16& text, std::size_t pos, const char16_t* word) {
    std::size_t i = 0;
    for (; word[i] != 0; ++i) {
        if (pos + i >= text.size() || !fold_ascii(text[pos + i], word[i])) return false;
    }
    return true;
}

bool is_ascii_digit(char16_t c) { return c >= u'0' && c <= u'9'; }

bool is_ascii_letter(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

/** The shared `turn\d+[a-z]+\d+` chunk (case-insensitive); end offset or npos. Greedy runs
 *  cannot backtrack into a valid match, exactly like the pinned copy in citations.cpp. */
std::size_t match_turn_group(const U16& text, std::size_t pos) {
    if (pos + 4 > text.size()) return kNpos;
    if (!starts_with_fold(text, pos, u"turn")) return kNpos;
    std::size_t p = pos + 4;
    const std::size_t digits1 = p;
    while (p < text.size() && is_ascii_digit(text[p])) ++p;
    if (p == digits1) return kNpos;
    const std::size_t letters = p;
    while (p < text.size() && is_ascii_letter(text[p])) ++p;
    if (p == letters) return kNpos;
    const std::size_t digits2 = p;
    while (p < text.size() && is_ascii_digit(text[p])) ++p;
    if (p == digits2) return kNpos;
    return p;
}

/** Kotlin String.trim()/isBlank() whitespace (Character.isWhitespace || isSpaceChar). */
bool is_kotlin_space(char16_t unit) {
    if (unit > 0x7F) {
        return json::is_kotlin_whitespace_code_point(static_cast<std::uint32_t>(unit));
    }
    return json::is_kotlin_whitespace_code_point(static_cast<std::uint32_t>(
        static_cast<unsigned char>(unit)));
}

U16 trim_units(const U16& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_kotlin_space(text[begin])) ++begin;
    while (end > begin && is_kotlin_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::string to_utf8(const U16& text) { return utf16::to_utf8(text); }
U16 from_utf8(std::string_view text) { return utf16::from_utf8(text); }

std::size_t utf16_index_of(const U16& haystack, const U16& needle, std::size_t from) {
    if (needle.empty() || needle.size() > haystack.size()) return kNpos;
    for (std::size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        if (haystack.compare(i, needle.size(), needle) == 0) return i;
    }
    return kNpos;
}

/** Kotlin String.lastIndexOf(needle, startIndex) — the last occurrence starting at <= from. */
std::size_t utf16_last_index_of(const U16& haystack, const U16& needle, std::size_t from) {
    if (haystack.empty() || needle.empty()) return kNpos;
    if (from >= haystack.size()) from = haystack.size() - 1;
    if (needle.size() > from + 1) return kNpos;
    for (std::size_t i = from + 1 - needle.size();; --i) {
        if (haystack.compare(i, needle.size(), needle) == 0) return i;
        if (i == 0) break;
    }
    return kNpos;
}

// ------------------------------------------------------------------------ marker regexes

struct PendingMarker {
    std::size_t answer_end = 0;  // UTF-16 length of `cleaned` when the marker was consumed
    std::string kind;
    std::string source_id;
};

struct MarkerMatch {
    std::size_t end = 0;
    std::string kind;  // "cite" / "filecite" (alt-1) or "" (alt-2)
    std::vector<std::string> ids;
};

/** `\uE200(cite|filecite)\uE202([^\uE200\uE201]+)\uE201` at exactly `pos`. */
bool match_envelope(const U16& text, std::size_t pos, MarkerMatch* out) {
    const std::size_t n = text.size();
    if (pos >= n || text[pos] != kPua200) return false;
    std::size_t p = pos + 1;
    std::string kind;
    if (starts_with_fold(text, p, u"cite")) {
        kind = "cite";
        p += 4;
    } else if (starts_with_fold(text, p, u"filecite")) {
        kind = "filecite";
        p += 8;
    } else {
        return false;
    }
    if (p >= n || text[p] != kPua202) return false;
    ++p;
    const std::size_t content_begin = p;
    while (p < n && text[p] != kPua200 && text[p] != kPua201) ++p;
    if (p == content_begin) return false;  // [^...]+ needs at least one unit
    if (p >= n || text[p] != kPua201) return false;

    const U16 content = text.substr(content_begin, p - content_begin);
    std::vector<std::string> ids;
    if (!json::is_blank(to_utf8(content))) {  // Kotlin: groupValues[2].takeIf(isNotBlank)
        std::size_t begin = 0;
        for (;;) {
            const std::size_t sep = content.find(kPua202, begin);
            const U16 part = content.substr(
                begin, sep == kNpos ? kNpos : sep - begin);
            const U16 trimmed = trim_units(part);
            if (!trimmed.empty()) ids.push_back(to_utf8(trimmed));
            if (sep == kNpos) break;
            begin = sep + 1;
        }
    }
    out->end = p + 1;
    out->kind = std::move(kind);
    out->ids = std::move(ids);
    return true;
}

/** 【(turn\d+[a-z]+\d+)】 at exactly `pos`. */
bool match_cjk_marker(const U16& text, std::size_t pos, MarkerMatch* out) {
    const std::size_t n = text.size();
    if (pos >= n || text[pos] != kCjkOpen) return false;
    const std::size_t group_end = match_turn_group(text, pos + 1);
    if (group_end == kNpos || group_end >= n || text[group_end] != kCjkClose) return false;
    out->end = group_end + 1;
    out->kind = "";
    out->ids = {to_utf8(text.substr(pos + 1, group_end - pos - 1))};
    return true;
}

/** Kotlin findAll of the two alternatives: leftmost, non-overlapping, alt-1 before alt-2. */
bool next_marker(const U16& text, std::size_t from, std::size_t* match_begin, MarkerMatch* match) {
    for (std::size_t i = from; i < text.size(); ++i) {
        if (match_envelope(text, i, match) || match_cjk_marker(text, i, match)) {
            *match_begin = i;
            return true;
        }
    }
    return false;
}

/** `privateSourceId`: `turn\d+[a-z]+\d+` findAll, case-insensitive, non-overlapping. */
void collect_turn_ids(const std::string& content, std::vector<std::string>* ids) {
    const U16 units = from_utf8(content);
    std::size_t i = 0;
    while (i < units.size()) {
        const std::size_t end = match_turn_group(units, i);
        if (end == kNpos) {
            ++i;
            continue;
        }
        const std::string id = to_utf8(units.substr(i, end - i));
        if (std::find(ids->begin(), ids->end(), id) == ids->end()) ids->push_back(id);
        i = end;
    }
}

// ---------------------------------------------------------------------- reference walking

std::vector<std::pair<std::string, const Value*>> object_entries(const Value& object) {
    std::vector<std::pair<std::string, const Value*>> entries;
    for (const json::Member& member : object.members) {
        bool replaced = false;
        for (auto& entry : entries) {
            if (entry.first == member.key) {
                entry.second = &member.value;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.emplace_back(member.key, &member.value);
    }
    return entries;
}

void add_id(std::vector<std::string>& ids, const std::string& id) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
}

void collect_private_ids(const Value& node, std::vector<std::string>* ids) {
    if (node.kind == Kind::Array) {
        for (const Value& element : node.elements) collect_private_ids(element, ids);
        return;
    }
    if (node.kind == Kind::Object) {
        for (const auto& entry : object_entries(node)) collect_private_ids(*entry.second, ids);
        return;
    }
    if (node.kind == Kind::String) collect_turn_ids(node.text, ids);
}

std::optional<int> primitive_int(const Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::String && value->kind != Kind::Literal) return std::nullopt;
    return int_or_null(value->text);
}

std::optional<std::string> primitive_string(const Value* value) {
    if (value == nullptr || value->kind != Kind::String) return std::nullopt;
    if (json::is_blank(value->text)) return std::nullopt;
    return value->text;
}

const Value* member(const Value& object, std::string_view key) {
    const Value* found = nullptr;
    for (const auto& entry : object_entries(object)) {
        if (entry.first == key) found = entry.second;
    }
    return found;
}

void collect_structured_ref_ids(const Value& node, std::vector<std::string>* ids) {
    if (node.kind == Kind::Array) {
        for (const Value& element : node.elements) collect_structured_ref_ids(element, ids);
        return;
    }
    if (node.kind != Kind::Object) return;
    const std::optional<int> turn = primitive_int(member(node, "turn_index"));
    const std::optional<std::string> type = primitive_string(member(node, "ref_type"));
    const std::optional<int> index = primitive_int(member(node, "ref_index"));
    if (turn.has_value() && type.has_value() && index.has_value()) {
        add_id(*ids, "turn" + std::to_string(*turn) + *type + std::to_string(*index));
    }
    for (const auto& entry : object_entries(node)) collect_structured_ref_ids(*entry.second, ids);
}

std::optional<std::string> find_named_string(const Value& node,
                                             const std::vector<std::string>& names) {
    if (node.kind == Kind::Array) {
        for (const Value& element : node.elements) {
            const auto found = find_named_string(element, names);
            if (found.has_value()) return found;
        }
        return std::nullopt;
    }
    if (node.kind != Kind::Object) return std::nullopt;
    const auto entries = object_entries(node);
    for (const std::string& name : names) {
        for (const auto& entry : entries) {
            if (entry.first != name) continue;
            const auto found = primitive_string(entry.second);
            if (found.has_value()) return found;
        }
    }
    for (const auto& entry : entries) {
        const auto found = find_named_string(*entry.second, names);
        if (found.has_value()) return found;
    }
    return std::nullopt;
}

std::optional<int> find_named_int(const Value& node, const std::vector<std::string>& names) {
    if (node.kind == Kind::Array) {
        for (const Value& element : node.elements) {
            const auto found = find_named_int(element, names);
            if (found.has_value()) return found;
        }
        return std::nullopt;
    }
    if (node.kind != Kind::Object) return std::nullopt;
    const auto entries = object_entries(node);
    for (const std::string& name : names) {
        for (const auto& entry : entries) {
            if (entry.first != name) continue;
            const auto found = primitive_int(entry.second);
            if (found.has_value()) return found;
        }
    }
    for (const auto& entry : entries) {
        const auto found = find_named_int(*entry.second, names);
        if (found.has_value()) return found;
    }
    return std::nullopt;
}

/** findSafeUrlCandidate: the special `safe_urls` array first (any primitive contributes its raw
 *  content — a number or even null!), then the recursive walk. */
std::optional<std::string> find_safe_url_candidate(const Value& node) {
    if (node.kind == Kind::Array) {
        for (const Value& element : node.elements) {
            const auto found = find_safe_url_candidate(element);
            if (found.has_value()) return found;
        }
        return std::nullopt;
    }
    if (node.kind != Kind::Object) return std::nullopt;
    const auto entries = object_entries(node);
    for (const auto& entry : entries) {
        if (entry.first != "safe_urls" || entry.second->kind != Kind::Array) continue;
        for (const Value& element : entry.second->elements) {
            if (element.kind == Kind::String || element.kind == Kind::Literal) return element.text;
        }
        break;  // special branch present but without a primitive -> fall through to the walk
    }
    for (const auto& entry : entries) {
        const auto found = find_safe_url_candidate(*entry.second);
        if (found.has_value()) return found;
    }
    return std::nullopt;
}

struct ImportedSource {
    std::vector<std::string> ids;
    std::optional<std::string> title;
    std::optional<std::string> url;
    std::optional<std::string> file_name;
    std::optional<std::string> location;
    std::optional<std::string> excerpt;
    std::optional<std::string> matched_text;
    std::optional<int> start_index;
    std::optional<int> end_index;
};

std::optional<ImportedSource> to_imported_source(const Value& node) {
    ImportedSource source;
    collect_private_ids(node, &source.ids);
    collect_structured_ref_ids(node, &source.ids);
    if (const auto uuid = find_named_string(node, {"uuid", "source_id"})) {
        add_id(source.ids, *uuid);
    }
    source.title = find_named_string(node, {"title", "alt", "attribution", "name"});
    source.url = find_named_string(node, {"url", "uri", "link"});
    if (!source.url.has_value()) source.url = find_safe_url_candidate(node);
    source.file_name = find_named_string(node, {"file_name", "filename"});
    source.location = find_named_string(node, {"location"});
    source.excerpt = find_named_string(node, {"snippet", "excerpt", "cited_text"});
    source.matched_text = find_named_string(node, {"matched_text"});
    source.start_index = find_named_int(node, {"start_idx", "start_index"});
    source.end_index = find_named_int(node, {"end_idx", "end_index"});
    if (source.ids.empty() && !source.url.has_value() && !source.file_name.has_value() &&
        !source.title.has_value()) {
        return std::nullopt;
    }
    return source;
}

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// ------------------------------------------------------------------------- anchor resolution

std::optional<citations::CitationAnchor> resolve_imported_anchor(const ImportedSource& source,
                                                                 const U16& answer,
                                                                 std::size_t marker_end) {
    std::optional<U16> matched;
    if (source.matched_text.has_value() && !source.matched_text->empty()) {
        matched = from_utf8(*source.matched_text);
    }
    if (matched.has_value() && source.start_index.has_value() && source.end_index.has_value()) {
        const int start = *source.start_index;
        const int end = *source.end_index;
        if (start >= 0 && static_cast<std::size_t>(end) <= answer.size() &&
            end > start && static_cast<std::size_t>(start) <= answer.size() &&
            answer.compare(static_cast<std::size_t>(start),
                           static_cast<std::size_t>(end - start), *matched) == 0) {
            return citations::CitationAnchor{
                start, end, source.matched_text.value_or(std::string())};
        }
    }
    if (!matched.has_value() || marker_end > answer.size()) return std::nullopt;
    const std::size_t start = utf16_last_index_of(answer, *matched, marker_end);
    if (start == kNpos) return std::nullopt;
    const std::size_t end = start + matched->size();
    if (end > marker_end) return std::nullopt;
    const std::string gap = to_utf8(answer.substr(end, marker_end - end));
    if (!json::is_blank(gap)) return std::nullopt;
    return citations::CitationAnchor{static_cast<int>(start), static_cast<int>(end),
                                     source.matched_text.value_or(std::string())};
}

std::optional<citations::CitationRecord> to_citation_record(
    const ImportedSource& source, std::string_view provider, const std::optional<std::string>& private_id,
    bool force_file, const U16& answer, std::size_t marker_end,
    const std::optional<std::int32_t>& anchor_start_override) {
    if (!source.url.has_value() && !source.file_name.has_value() && !source.title.has_value()) {
        return std::nullopt;
    }
    std::optional<citations::CitationAnchor> anchor;
    if (anchor_start_override.has_value() && !answer.empty()) {
        anchor = citations::CitationAnchor{
            *anchor_start_override,
            *anchor_start_override + static_cast<std::int32_t>(answer.size()),
            to_utf8(answer)};
    } else {
        anchor = resolve_imported_anchor(source, answer, marker_end);
    }

    std::string kind;
    if (force_file || source.file_name.has_value()) {
        kind = "file";
    } else if (policy::safe_http_url(source.url).has_value()) {
        kind = "url";
    } else {
        kind = "document";
    }
    std::vector<citations::CitationAnchor> anchors;
    if (anchor.has_value()) anchors.push_back(*anchor);
    return policy::create(std::string(provider), kind, source.title, source.url, source.file_name,
                          source.location, private_id, source.excerpt, anchors);
}

}  // namespace

ProjectedText project_chatgpt_citations(std::string_view raw_text,
                                        const std::vector<json::Value>& references) {
    const U16 raw = from_utf8(raw_text);

    // 1. Clean the markers, recording one pending entry per split id.
    U16 cleaned;
    std::vector<PendingMarker> pending;
    std::size_t cursor = 0;
    std::size_t marker_begin = 0;
    MarkerMatch match;
    while (next_marker(raw, cursor, &marker_begin, &match)) {
        cleaned += raw.substr(cursor, marker_begin - cursor);
        if (match.kind.empty()) match.kind = "cite";  // Kotlin: groupValues[1].ifBlank { "cite" }
        for (const std::string& id : match.ids) {
            pending.push_back(PendingMarker{cleaned.size(), match.kind, id});
        }
        cursor = match.end;
    }
    cleaned += raw.substr(cursor);

    ProjectedText out;
    out.text = policy::strip_private_markers(to_utf8(cleaned));
    const U16 answer = from_utf8(out.text);

    // 2. Index the references by id (first source wins, keys lowercased like Kotlin).
    std::vector<std::pair<std::string, ImportedSource>> sources;
    for (const json::Value& reference : references) {
        const auto source = to_imported_source(reference);
        if (source.has_value()) sources.push_back({std::string(), *source});
    }
    std::vector<std::pair<std::string, const ImportedSource*>> by_id;
    for (const auto& entry : sources) {
        for (const std::string& id : entry.second.ids) {
            const std::string key = ascii_lower(id);
            bool present = false;
            for (const auto& existing : by_id) {
                if (existing.first == key) {
                    present = true;
                    break;
                }
            }
            if (!present) by_id.emplace_back(key, &entry.second);
        }
    }

    // 3. Resolve each pending marker against the id index.
    std::vector<citations::CitationRecord> citations;
    for (const PendingMarker& marker : pending) {
        const std::string key = ascii_lower(marker.source_id);
        const ImportedSource* source = nullptr;
        for (const auto& entry : by_id) {
            if (entry.first == key) {
                source = entry.second;
                break;
            }
        }
        if (source == nullptr) continue;  // mapNotNull { sourcesById[id] ?: return null }
        const bool force_file = ascii_lower(marker.kind) == "filecite";
        auto record = to_citation_record(*source, "openai", marker.source_id, force_file, answer,
                                         marker.answer_end, std::nullopt);
        if (record.has_value()) citations.push_back(std::move(*record));
    }

    out.citations = policy::deduplicate(citations, std::optional<std::string>(out.text));
    return out;
}

std::optional<citations::CitationRecord> project_claude_citation(
    const json::Value& citation, std::int32_t answer_start_index, std::string_view answer_text) {
    const auto source = to_imported_source(citation);
    if (!source.has_value()) return std::nullopt;
    const std::optional<std::string> private_id =
        source->ids.empty() ? std::nullopt : std::optional<std::string>(source->ids.front());
    const U16 answer = from_utf8(answer_text);
    return to_citation_record(*source, "anthropic", private_id,
                              source->file_name.has_value(), answer, answer.size(),
                              answer_start_index);
}

std::optional<std::string> encode_imported_citations(
    const std::vector<citations::CitationRecord>& citations, std::string_view answer_text) {
    const std::vector<citations::CitationRecord> deduplicated =
        policy::deduplicate(citations, std::optional<std::string>(answer_text));
    std::vector<backup::MessageSegment> segments;
    segments.reserve(deduplicated.size());
    for (const citations::CitationRecord& record : deduplicated) {
        backup::MessageSegment segment;
        segment.type = "citation";
        segment.content = policy::encode(record);
        segments.push_back(std::move(segment));
    }
    return message_persistence_guard::encode_segments_bounded(segments);
}

}  // namespace agora::core::imported_citation
