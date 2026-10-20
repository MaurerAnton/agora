#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agora::core::v18_regeneration_repair {

// Port of data/local/migration/V18RegenerationEdgeRepairPlanner.kt: the forward repair for the
// rejected first v17 -> v18 data migration. It repairs exactly one partial shape and leaves every
// historical orphan edge untouched.

enum class Participant {
    User,
    Model,
    Error,
};

struct RunRecord {
    std::string id;
    std::optional<std::string> parent_run_id;
    bool legacy_ambiguous = false;
};

struct MessageRecord {
    std::string id;
    std::optional<std::string> parent_id;
    Participant participant = Participant::User;
    std::string run_id;
    long long run_sequence = 0;
};

/** Prefixes from util/Constants.kt that make a message row non-ordinary. */
inline constexpr char kToolMsgPrefix[] = "tool_";
inline constexpr char kResultMsgPrefix[] = "result_";
inline constexpr char kCompactMsgPrefix[] = "compact_";
inline constexpr char kToolCallIdPrefix[] = "call_";

/**
 * plan: returns the message-id -> canonical-user-id repairs in the exact order the Kotlin
 * LinkedHashMap emits them (runs in input order; dangling siblings sorted by runSequence, then
 * id; a repeated message id keeps its first insertion position but takes the latest value).
 *
 * A run is repaired only when it is legacy-ambiguous, its parent run exists, the parent still has
 * an ordinary USER child at sequence zero, and the run already has an ordinary MODEL child rooted
 * at that user. All conditions are required; orphan edges outside that shape are not touched.
 */
std::vector<std::pair<std::string, std::string>> plan(const std::vector<RunRecord>& runs,
                                                      const std::vector<MessageRecord>& messages);

}  // namespace agora::core::v18_regeneration_repair
