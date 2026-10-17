#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"

// A C++ port of the citation pipeline: the CitationPolicy object plus the Anthropic and Gemini
// citation projections (core/tools/golden/citations). Every rule the golden traces pin is
// reproduced literally, including the kotlinx serialization details of encode/decode.
//
// Boundaries (documented divergences, none of them reachable from the frozen corpus):
//  * Case folding (provider/kind normalization, matchesCitationTitle) is ASCII-only; the
//    exotic Unicode foldings are out of scope, consistent with the rest of the core port.
//  * toIntOrNull accepts ASCII digits only (the JVM's Character.digit would also accept
//    Arabic-Indic and other Unicode digits).
//  * Text is UTF-8 at the boundary; lone surrogates cannot be represented (anchors that would
//    split a surrogate pair are excluded from the corpus).
namespace agora::core::citations {

struct CitationAnchor {
    std::int32_t start_index = 0;
    std::int32_t end_index = 0;
    std::string cited_text;
};

bool operator==(const CitationAnchor& first, const CitationAnchor& second);
bool operator!=(const CitationAnchor& first, const CitationAnchor& second);

struct CitationRecord {
    std::int32_t version = 1;
    std::string source_id;
    std::string provider;
    std::string kind;
    std::string title;
    std::optional<std::string> url;
    std::optional<std::string> file_name;
    std::optional<std::string> location;
    std::optional<std::string> provider_source_id;
    std::optional<std::string> excerpt;
    std::vector<CitationAnchor> anchors;
};

bool operator==(const CitationRecord& first, const CitationRecord& second);
bool operator!=(const CitationRecord& first, const CitationRecord& second);

namespace policy {

constexpr std::int32_t kVERSION = 1;
constexpr std::size_t kMaxSources = 99;
constexpr std::size_t kMaxAnchorsPerSource = 32;
constexpr std::size_t kMaxMetadataChars = 512;
constexpr std::size_t kMaxUrlChars = 4096;
constexpr std::size_t kMaxCitedTextChars = 4096;
constexpr std::size_t kMaxExcerptChars = 8192;

/** CitationPolicy.create. */
std::optional<CitationRecord> create(
    const std::string& provider,
    const std::string& kind,
    const std::optional<std::string>& title = std::nullopt,
    const std::optional<std::string>& url = std::nullopt,
    const std::optional<std::string>& file_name = std::nullopt,
    const std::optional<std::string>& location = std::nullopt,
    const std::optional<std::string>& provider_source_id = std::nullopt,
    const std::optional<std::string>& excerpt = std::nullopt,
    const std::vector<CitationAnchor>& anchors = {},
    const std::optional<std::string>& answer_text = std::nullopt);

/** CitationPolicy.normalize: version check plus a full create() round-trip. */
std::optional<CitationRecord> normalize_record(
    const CitationRecord& record,
    const std::optional<std::string>& answer_text = std::nullopt);

/** CitationPolicy.merge; throws std::invalid_argument when the source ids differ (Kotlin's
 *  require). */
CitationRecord merge(const CitationRecord& first, const CitationRecord& incoming);

/** CitationPolicy.encode: kotlinx encodeToString with encodeDefaults=false. */
std::string encode(const CitationRecord& record);

/** CitationPolicy.decode: strict kotlinx streaming parse of the record, then normalize. */
std::optional<CitationRecord> decode(
    std::string_view raw,
    const std::optional<std::string>& answer_text = std::nullopt);

/** CitationPolicy.safeHttpUrl: java.net.URI based URL normalization. */
std::optional<std::string> safe_http_url(const std::optional<std::string>& raw);

/** CitationPolicy.stripPrivateMarkers. */
std::string strip_private_markers(std::string_view text);

/** CitationPolicy.copyText. */
std::string copy_text(std::string_view answer, const std::vector<CitationRecord>& records);

/** CitationPolicy.deduplicate: per-source merge in insertion order, capped at 99 sources. */
std::vector<CitationRecord> deduplicate(
    const std::vector<CitationRecord>& records,
    const std::optional<std::string>& answer_text = std::nullopt);

}  // namespace policy

/** AnthropicCitationProjection.toAnthropicCitationRecord. */
std::optional<CitationRecord> to_anthropic_citation_record(const json::Value& element,
                                                           std::int32_t answer_start_index,
                                                           const std::string& answer_text);

/** GeminiCitationProjection.toGeminiCitations; the element must be a JsonObject. */
std::vector<CitationRecord> to_gemini_citations(const json::Value& element,
                                                const std::string& answer_text);

/** Iterable<CitationRecord>.matchesCitationTitle. */
bool matches_citation_title(const std::vector<CitationRecord>& records, const std::string& query);

}  // namespace agora::core::citations
