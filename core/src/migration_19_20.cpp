// Port of data/local/migration/Migration19To20.kt.
//
// Three SQL-only repair statements run inside Room's migration transaction. They must never read
// the oversized values through a cursor, so the port only emits the same execSQL calls with the
// same Kotlin argument types (Int limits, String marker).
#include "agora/core/migration_19_20.hpp"

#include <string>
#include <vector>

namespace agora::core::migration_19_20 {
namespace {

// Constants.MAX_PERSISTED_SEGMENTS_BYTES / MAX_PERSISTED_TEXT_CHARS.
constexpr int kMaxPersistedSegmentsBytes = 600'000;
constexpr int kMaxPersistedTextChars = 100'000;

// MessagePersistenceGuard.TRUNCATION_MARKER.
constexpr char kTruncationMarker[] = "\n…[truncated for persistence]";

constexpr char kClearOversizedToolCalls[] = R"(UPDATE messages
SET toolCallJson = NULL
WHERE toolCallJson IS NOT NULL
  AND length(CAST(toolCallJson AS BLOB)) > ?)";

constexpr char kTruncateOversizedText[] = R"(UPDATE messages
SET text = substr(text, 1, ?) || ?
WHERE length(text) > ?)";

constexpr char kTruncateOversizedThoughts[] = R"(UPDATE messages
SET thoughts = substr(thoughts, 1, ?) || ?
WHERE thoughts IS NOT NULL
  AND length(thoughts) > ?)";

}  // namespace

void migrate_19_20(Database& database) {
    database.exec_sql(kClearOversizedToolCalls,
                      {SqlValue::int_value(kMaxPersistedSegmentsBytes)});
    database.exec_sql(kTruncateOversizedText,
                      {SqlValue::int_value(kMaxPersistedTextChars),
                       SqlValue::text_value(kTruncationMarker),
                       SqlValue::int_value(kMaxPersistedTextChars)});
    database.exec_sql(kTruncateOversizedThoughts,
                      {SqlValue::int_value(kMaxPersistedTextChars),
                       SqlValue::text_value(kTruncationMarker),
                       SqlValue::int_value(kMaxPersistedTextChars)});
}

}  // namespace agora::core::migration_19_20
