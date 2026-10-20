#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/backup_media_policy.hpp"

namespace agora::core::message_persistence_guard {

/** Port of model/MessagePersistenceGuard.kt. */
inline constexpr int kMaxPersistedSegmentsBytes = 600000;
inline constexpr int kMaxPersistedTextChars = 100000;

/** The exact marker appended to every trimmed field. */
inline constexpr std::string_view kTruncationMarker =
    "\n\xE2\x80\xA6[truncated for persistence]";

/**
 * Kotlin's `error(...)` in oversizedResult: the caller must not turn protected Responses
 * continuation state into SQL NULL, because that would leave tool outputs without the items
 * needed to rebuild the next request.
 */
class PersistenceBudgetError : public std::runtime_error {
   public:
    explicit PersistenceBudgetError(const std::string& message) : std::runtime_error(message) {}
};

/** clipText: cap a persisted answer/reasoning column without splitting a surrogate pair. */
std::string clip_text(std::string_view text);

/**
 * encodeSegmentsBounded: return the canonical kotlinx JSON when it fits, otherwise halve the
 * largest trimmable field repeatedly until the encoded row fits. nullopt is SQL NULL.
 *
 * Throws PersistenceBudgetError when the row cannot fit and any segment carries
 * responseOutputItems. Segments are the same model as the backup codec, whose encoder is pinned
 * against kotlinx by the backup-media-policy stand.
 */
std::optional<std::string> encode_segments_bounded(
    const std::vector<backup::MessageSegment>& segments,
    int max_bytes = kMaxPersistedSegmentsBytes);

}  // namespace agora::core::message_persistence_guard
