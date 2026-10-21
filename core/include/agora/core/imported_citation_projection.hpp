#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/citations.hpp"
#include "agora/core/json.hpp"

namespace agora::core::imported_citation {

/**
 * Port of data/ImportedCitationProjection.kt: the citation half of the ChatGPT/Claude import
 * transforms. The three functions are exactly what the file exports:
 *
 *  projectChatGptCitations — strips the private \uE200cite\uE202…\uE201 / 【turn…】 markers from
 *      the raw answer, resolves each marker id against the export's reference list, anchors each
 *      record against the *stripped* answer and deduplicates through CitationPolicy.
 *  projectClaudeCitation — one reference element into a CitationRecord, anchored from the caller's
 *      block offset.
 *  encodeImportedCitations — deduplicate + toMessageSegment + the persistence-guard bounded
 *      encoder (nullopt = SQL NULL).
 *
 * Indexes (anchor start/end, markerEnd, answerStartIndex) are Kotlin String indexes, i.e. UTF-16
 * code units; the port converts to UTF-16 for every position-sensitive step and returns UTF-8.
 * All helpers (CitationPolicy, MessagePersistenceGuard, MessageSegment) are already pinned by
 * their own stands — nothing here reimplements them.
 */

struct ProjectedText {
    std::string text;
    std::vector<citations::CitationRecord> citations;
};

/** projectChatGptCitations(rawText, references). */
ProjectedText project_chatgpt_citations(std::string_view raw_text,
                                        const std::vector<json::Value>& references);

/** projectClaudeCitation(citation, answerStartIndex, answerText). */
std::optional<citations::CitationRecord> project_claude_citation(
    const json::Value& citation, std::int32_t answer_start_index, std::string_view answer_text);

/** encodeImportedCitations(citations, answerText): nullopt when the row does not fit. */
std::optional<std::string> encode_imported_citations(
    const std::vector<citations::CitationRecord>& citations, std::string_view answer_text);

}  // namespace agora::core::imported_citation
