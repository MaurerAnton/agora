#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::regeneration_tree_repair {

// Port of data/local/migration/RegenerationTreeRepairPlanner.kt: the forward repair for the v17
// shape where Regenerate and Edit were both persisted as sibling USER inputs. The exact repeated
// boundary payload (input fingerprint) is the only durable discriminator: the earliest sibling is
// the shared user anchor, later siblings are inferred regenerations whose leading cloned inputs
// are deleted and whose root assistant is attached to the anchor user.

enum class Participant {
    User,
    Model,
    Error,
};

struct V17RunRecord {
    std::string id;
    std::optional<std::string> parent_run_id;
    long long started_at = 0;
};

struct V17MessageRecord {
    std::string id;
    std::optional<std::string> parent_id;
    Participant participant = Participant::User;
    long long timestamp = 0;
    std::string run_id;
    long long run_sequence = 0;
    std::string input_fingerprint;
};

/**
 * LinkedHashMap / LinkedHashSet ordering is part of the migration's emitted SQL:
 * every vector keeps Kotlin's insertion order (updates in place, deletions and run sequences
 * appended in the order the planner walks the runs).
 */
struct Plan {
    std::vector<std::pair<std::string, std::string>> run_parent_updates;
    std::vector<std::string> deleted_message_ids;
    std::vector<std::pair<std::string, std::string>> message_parent_updates;
    std::vector<std::pair<std::string, long long>> run_sequence_updates;
    std::vector<std::pair<std::optional<std::string>, std::string>> message_selections;
    std::vector<std::pair<std::optional<std::string>, std::string>> run_selections;
    std::vector<std::string> inferred_run_ids;
};

/** Synthetic rows are never boundaries, roots or leading inputs. */
inline constexpr char kToolMsgPrefix[] = "tool_";
inline constexpr char kResultMsgPrefix[] = "result_";

Plan plan(const std::vector<V17RunRecord>& runs,
          const std::vector<V17MessageRecord>& messages,
          const std::vector<std::pair<std::optional<std::string>, std::string>>& message_selections,
          const std::vector<std::pair<std::optional<std::string>, std::string>>& run_selections);

/**
 * regenerationInputFingerprint: SHA-256 over `text\0imageCount\0normalizedAttachment`, where the
 * attachment metadata is JSON-sanitized (every `originalUri` key removed recursively, otherwise
 * re-serialized exactly like kotlinx JsonElement.toString()) and left raw when it is blank or not
 * valid JSON.
 */
std::string regeneration_input_fingerprint(std::string_view text, int image_count,
                                           const std::optional<std::string>& attachment_meta);

}  // namespace agora::core::regeneration_tree_repair
