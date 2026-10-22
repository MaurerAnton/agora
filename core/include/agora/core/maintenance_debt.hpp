#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/run_branch_selection.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::maintenance_debt {

/**
 * Port of data/local/MaintenanceDebt.kt: the debt queue behind every background maintenance pass
 * (orphan sweeping, run-branch repair) with its claim/complete/release cycle and the
 * compare-and-set conversation repair.
 *
 * The SQL text comes from the @Query annotations, the @Transaction bodies are ported verbatim,
 * and Room itself is replaced by the SqlExecutor boundary. `System.currentTimeMillis()` inside
 * repairRunBranches is the one clock read: the port takes it as a parameter.
 */

/** MaintenanceDebtEntity (kind+identity composite key), validated on construction. */
struct DebtEntry {
    std::string kind;
    std::string identity;
    std::string state = "PENDING";
    std::int64_t revision = 1;
    std::int64_t updated_at = 0;
    std::optional<std::string> claim_id;
    std::optional<std::int64_t> claimed_at;

    /** The entity's init block: every `require` of MaintenanceDebtEntity. */
    void validate() const;
};

/** The conversation row repairRunBranches reads (the stand surface, see the golden stand). */
struct ConversationEntry {
    std::string id;
    std::optional<std::string> selected_run_branches_json;
};

/** RunEntity (data/local/RunEntity.kt): enum columns by name, init validated per row. */
struct RunEntry {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_run_id;
    std::string status;
    std::optional<std::int64_t> active_slot;
    std::int64_t started_at = 0;
    std::int64_t last_checkpoint_at = 0;
    std::optional<std::int64_t> stop_requested_at;
    std::optional<std::int64_t> ended_at;
    std::optional<std::string> end_reason;
    std::int64_t current_pass = 0;
    bool legacy_ambiguous = false;

    /** Enum name resolution (Java's Enum.valueOf failure text) plus the init requires. */
    void validate() const;
};

/** The run-selection map: maintenance and recovery share the JSON round trip (run_branch_selection). */
using RunSelections = agora::core::run_branch_selection::SelectionMap;

/** The ChatDao-independent maintenance queue over a SqlExecutor (Room replaced by the boundary). */
class MaintenanceDebtDao {
public:
    explicit MaintenanceDebtDao(SqlExecutor& sql) : sql_(sql), stack_(sql) {}

    // -- @Transaction nesting ---------------------------------------------------------------

    void begin_transaction();
    void set_transaction_successful();
    void end_transaction();

    // -- plain DAO methods ------------------------------------------------------------------

    /** @Insert(onConflict = IGNORE): -1 when the row was ignored. */
    std::int64_t insert_debt(const DebtEntry& debt);

    std::int64_t renew_debt(std::string_view kind, std::string_view identity,
                            std::int64_t updated_at);

    std::vector<DebtEntry> get_claim_candidates(std::int64_t stale_before, std::int32_t limit);

    std::int64_t claim_candidate(std::string_view kind, std::string_view identity,
                                 std::int64_t revision, std::string_view claim_id,
                                 std::int64_t claimed_at, std::int64_t stale_before);

    bool has_debt();

    std::vector<std::int64_t> get_orphan_embedding_ids_page(std::int64_t after_id,
                                                           std::int32_t limit);

    std::int64_t delete_orphan_embeddings_for_message(std::string_view message_id);

    std::int64_t delete_orphan_embeddings_by_ids(const std::vector<std::int64_t>& ids);

    std::vector<std::string> get_run_branch_conversation_ids_page(
        const std::optional<std::string>& after_id, std::int32_t limit);

    std::optional<ConversationEntry> get_maintenance_conversation(std::string_view conversation_id);

    std::vector<RunEntry> get_maintenance_runs(std::string_view conversation_id);

    std::int64_t compare_and_set_maintenance_run_branches(std::string_view conversation_id,
                                                         std::string_view expected,
                                                         std::string_view replacement,
                                                         std::int64_t at);

    std::optional<DebtEntry> get_debt(std::string_view kind, std::string_view identity);

    std::int64_t complete_claimed(std::string_view kind, std::string_view identity,
                                  std::int64_t revision, std::string_view claim_id);

    std::int64_t release_claimed(std::string_view kind, std::string_view identity,
                                 std::int64_t revision, std::string_view claim_id,
                                 std::int64_t updated_at);

    // -- @Transaction default methods (bodies verbatim) -------------------------------------

    DebtEntry enqueue(std::string_view kind, std::string_view identity, std::int64_t updated_at);

    std::vector<DebtEntry> claim(std::string_view claim_id, std::int64_t claimed_at,
                                 std::int64_t stale_before, std::int32_t limit);

    bool complete(const DebtEntry& claim);

    bool release(const DebtEntry& claim, std::int64_t updated_at);

    /**
     * repairRunBranches. `now_ms` is the System.currentTimeMillis() the Kotlin body reads
     * inline; the caller supplies it because wall-clock time is a platform source.
     */
    bool repair_run_branches(std::string_view conversation_id, std::int64_t now_ms);

private:
    SqlExecutor& sql_;
    TransactionStack stack_;
};

}  // namespace agora::core::maintenance_debt
