// Port of data/local/MaintenanceDebt.kt.
//
// The debt queue is the coordination point of every background maintenance pass: workers enqueue
// rows, claim them with a lease (revision + claimId + staleBefore), and complete or release them
// with compare-and-set semantics so a lease lost to a newer revision is never double-applied.
// repairRunBranches round-trips the durable run-branch projection through the already-ported
// RunBranchSelectionIntegrity and writes it back only when the projection actually changes.
#include "agora/core/maintenance_debt.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/run_branch_selection.hpp"

namespace agora::core::maintenance_debt {

namespace {

namespace run_branch = agora::core::run_branch_selection;

constexpr std::string_view kStatePending = "PENDING";
constexpr std::string_view kStateClaimed = "CLAIMED";

/** MaintenanceDebtDao.INSERT_IGNORED */
constexpr std::int64_t kInsertIgnored = -1;

constexpr std::int32_t kMaxClaimBatch = 100;

bool is_blank(std::string_view text) { return json::is_blank(text); }

/** The debt SELECT column list, in entity declaration order. */
constexpr std::string_view kDebtColumns = "kind, identity, state, revision, updatedAt, claimId, claimedAt";

DebtEntry debt_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 7, "MaintenanceDebtDao: debt row shape mismatch");
    DebtEntry entry;
    entry.kind = row[0].as_text();
    entry.identity = row[1].as_text();
    entry.state = row[2].as_text();
    entry.revision = row[3].as_integer();
    entry.updated_at = row[4].as_integer();
    entry.claim_id = row[5].text_or_null();
    entry.claimed_at = row[6].integer_or_null();
    entry.validate();
    return entry;
}

/** RunStatus.isTerminal: COMPLETED | STOPPED | FAILED. */
bool is_terminal_status(std::string_view status) {
    return status == "COMPLETED" || status == "STOPPED" || status == "FAILED";
}

/**
 * Room maps enum columns with Enum.valueOf on the stored name; Java's failure text is
 * "No enum constant <canonical>.<name>".
 */
std::string enum_name_or_throw(const SqlValue& cell, std::string_view canonical_class,
                               std::initializer_list<std::string_view> constants) {
    const std::string& value = cell.as_text();
    for (std::string_view candidate : constants) {
        if (candidate == value) return value;
    }
    throw KotlinError("IllegalArgumentException",
                      "No enum constant " + std::string(canonical_class) + "." + value);
}

RunEntry run_from_row(const SqlRow& row) {
    // `SELECT * FROM runs` over the stand schema, in declaration/column order.
    kotlin_check(row.size() == 12, "MaintenanceDebtDao: run row shape mismatch");
    RunEntry entry;
    entry.id = row[0].as_text();
    entry.conversation_id = row[1].as_text();
    entry.parent_run_id = row[2].text_or_null();
    entry.status = enum_name_or_throw(
        row[3], "com.newoether.agora.model.RunStatus",
        {"ACTIVE", "STOPPING", "COMPLETED", "STOPPED", "FAILED"});
    entry.active_slot = row[4].integer_or_null();
    entry.started_at = row[5].as_integer();
    entry.last_checkpoint_at = row[6].as_integer();
    entry.stop_requested_at = row[7].integer_or_null();
    entry.ended_at = row[8].integer_or_null();
    if (!row[9].is_null()) {
        entry.end_reason = enum_name_or_throw(
            row[9], "com.newoether.agora.model.RunEndReason",
            {"MODEL_COMPLETED", "USER_STOPPED", "PROCESS_RECOVERED", "PROVIDER_ERROR"});
    }
    entry.current_pass = row[10].as_integer();
    entry.legacy_ambiguous = row[11].as_integer() != 0;
    entry.validate();
    return entry;
}

std::string placeholders(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out.push_back(',');
        out.push_back('?');
    }
    if (out.empty()) out = "NULL";  // Room's `IN (:ids)` with an empty list binds NULL
    return out;
}

/** withTransaction: the block marks the level successful unless it throws. */
template <typename Body>
auto run_in_transaction(MaintenanceDebtDao& dao, Body&& body) -> decltype(body()) {
    dao.begin_transaction();
    if constexpr (std::is_void_v<decltype(body())>) {
        try {
            body();
            dao.set_transaction_successful();
            dao.end_transaction();
        } catch (...) {
            dao.end_transaction();
            throw;
        }
    } else {
        try {
            auto result = body();
            dao.set_transaction_successful();
            dao.end_transaction();
            return result;
        } catch (...) {
            dao.end_transaction();
            throw;
        }
    }
}

}  // namespace

// ---- entity validation ---------------------------------------------------------------------

void DebtEntry::validate() const {
    kotlin_require(!is_blank(kind));
    kotlin_require(!is_blank(identity));
    kotlin_require(state == kStatePending || state == kStateClaimed);
    kotlin_require(revision > 0);
    kotlin_require(claim_id.has_value() == claimed_at.has_value());
    kotlin_require((state == kStateClaimed) == claim_id.has_value());
}

void RunEntry::validate() const {
    // RunEntity's init block, in source order.
    kotlin_require(current_pass >= 0);
    const bool terminal = is_terminal_status(status);
    kotlin_require((active_slot.has_value() && *active_slot == 1) == !terminal);
    kotlin_require(terminal == (ended_at.has_value() && end_reason.has_value()));
    kotlin_require(status != "STOPPING" || stop_requested_at.has_value());
}

// ---- run selection JSON --------------------------------------------------------------------

// ---- transactions ---------------------------------------------------------------------------

void MaintenanceDebtDao::begin_transaction() { stack_.begin(); }

void MaintenanceDebtDao::set_transaction_successful() { stack_.mark_successful(); }

void MaintenanceDebtDao::end_transaction() { stack_.end(); }

// ---- plain DAO methods ----------------------------------------------------------------------

std::int64_t MaintenanceDebtDao::insert_debt(const DebtEntry& debt) {
    debt.validate();
    static constexpr std::string_view kSql =
        "INSERT OR IGNORE INTO maintenance_debt "
        "(kind, identity, state, revision, updatedAt, claimId, claimedAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";
    SqlArgs args;
    args.push_back(SqlValue::text_value(debt.kind));
    args.push_back(SqlValue::text_value(debt.identity));
    args.push_back(SqlValue::text_value(debt.state));
    args.push_back(SqlValue::integer_value(debt.revision));
    args.push_back(SqlValue::integer_value(debt.updated_at));
    args.push_back(debt.claim_id.has_value() ? SqlValue::text_value(*debt.claim_id)
                                             : SqlValue::null());
    args.push_back(debt.claimed_at.has_value()
                       ? SqlValue::integer_value(*debt.claimed_at)
                       : SqlValue::null());
    const std::int64_t changed = sql_.execute(std::string(kSql), args);
    if (changed == 0) return kInsertIgnored;
    return sql_.last_insert_rowid();
}

std::int64_t MaintenanceDebtDao::renew_debt(std::string_view kind, std::string_view identity,
                                            std::int64_t updated_at) {
    static constexpr std::string_view kSql =
        "UPDATE maintenance_debt SET state = 'PENDING', revision = revision + 1, updatedAt = ?, "
        "claimId = NULL, claimedAt = NULL WHERE kind = ? AND identity = ?";
    return sql_.execute(std::string(kSql),
                        {SqlValue::integer_value(updated_at), SqlValue::text_value(std::string(kind)),
                         SqlValue::text_value(std::string(identity))});
}

std::vector<DebtEntry> MaintenanceDebtDao::get_claim_candidates(std::int64_t stale_before,
                                                               std::int32_t limit) {
    const std::string sql = std::string("SELECT ") + std::string(kDebtColumns) +
                            " FROM maintenance_debt WHERE state = 'PENDING' "
                            "OR (state = 'CLAIMED' AND claimedAt <= ?) "
                            "ORDER BY updatedAt, kind, identity LIMIT ?";
    std::vector<DebtEntry> out;
    for (const SqlRow& row : sql_.query(sql, {SqlValue::integer_value(stale_before),
                                              SqlValue::integer_value(limit)})) {
        out.push_back(debt_from_row(row));
    }
    return out;
}

std::int64_t MaintenanceDebtDao::claim_candidate(std::string_view kind, std::string_view identity,
                                                 std::int64_t revision, std::string_view claim_id,
                                                 std::int64_t claimed_at,
                                                 std::int64_t stale_before) {
    static constexpr std::string_view kSql =
        "UPDATE maintenance_debt SET state = 'CLAIMED', claimId = ?, claimedAt = ? "
        "WHERE kind = ? AND identity = ? AND revision = ? "
        "AND (state = 'PENDING' OR (state = 'CLAIMED' AND claimedAt <= ?))";
    SqlArgs args;
    args.push_back(SqlValue::text_value(std::string(claim_id)));
    args.push_back(SqlValue::integer_value(claimed_at));
    args.push_back(SqlValue::text_value(std::string(kind)));
    args.push_back(SqlValue::text_value(std::string(identity)));
    args.push_back(SqlValue::integer_value(revision));
    args.push_back(SqlValue::integer_value(stale_before));
    return sql_.execute(std::string(kSql), args);
}

bool MaintenanceDebtDao::has_debt() {
    std::vector<SqlRow> rows = sql_.query("SELECT EXISTS(SELECT 1 FROM maintenance_debt LIMIT 1)", {});
    kotlin_check(!rows.empty() && rows.front().size() == 1,
                 "MaintenanceDebtDao: EXISTS returned no row");
    return rows.front().front().as_integer() != 0;
}

std::vector<std::int64_t> MaintenanceDebtDao::get_orphan_embedding_ids_page(std::int64_t after_id,
                                                                          std::int32_t limit) {
    static constexpr std::string_view kSql =
        "SELECT id FROM embeddings WHERE id > ? "
        "AND (messageId LIKE 'compact_%' OR NOT EXISTS "
        "(SELECT 1 FROM messages WHERE messages.id = embeddings.messageId)) "
        "ORDER BY id LIMIT ?";
    std::vector<std::int64_t> out;
    for (const SqlRow& row : sql_.query(std::string(kSql),
                                        {SqlValue::integer_value(after_id),
                                         SqlValue::integer_value(limit)})) {
        out.push_back(row.front().as_integer());
    }
    return out;
}

std::int64_t MaintenanceDebtDao::delete_orphan_embeddings_for_message(std::string_view message_id) {
    static constexpr std::string_view kSql =
        "DELETE FROM embeddings WHERE messageId = ? AND NOT EXISTS "
        "(SELECT 1 FROM messages WHERE messages.id = embeddings.messageId)";
    return sql_.execute(std::string(kSql), {SqlValue::text_value(std::string(message_id))});
}

std::int64_t MaintenanceDebtDao::delete_orphan_embeddings_by_ids(
    const std::vector<std::int64_t>& ids) {
    if (ids.empty()) return 0;
    const std::string sql = "DELETE FROM embeddings WHERE id IN (" + placeholders(ids.size()) +
                            ") AND (messageId LIKE 'compact_%' OR NOT EXISTS "
                            "(SELECT 1 FROM messages WHERE messages.id = embeddings.messageId))";
    SqlArgs args;
    args.reserve(ids.size());
    for (std::int64_t id : ids) args.push_back(SqlValue::integer_value(id));
    return sql_.execute(sql, args);
}

std::vector<std::string> MaintenanceDebtDao::get_run_branch_conversation_ids_page(
    const std::optional<std::string>& after_id, std::int32_t limit) {
    static constexpr std::string_view kSql =
        "SELECT id FROM conversations WHERE selectedRunBranchesJson IS NOT NULL "
        "AND (? IS NULL OR id > ?) ORDER BY id LIMIT ?";
    SqlArgs args;
    args.push_back(after_id.has_value() ? SqlValue::text_value(*after_id) : SqlValue::null());
    args.push_back(after_id.has_value() ? SqlValue::text_value(*after_id) : SqlValue::null());
    args.push_back(SqlValue::integer_value(limit));
    std::vector<std::string> out;
    for (const SqlRow& row : sql_.query(std::string(kSql), args)) {
        out.push_back(row.front().as_text());
    }
    return out;
}

std::optional<ConversationEntry> MaintenanceDebtDao::get_maintenance_conversation(
    std::string_view conversation_id) {
    // `SELECT * FROM conversations WHERE id = :conversationId`; the stand schema's column order
    // maps positionally onto the conversation surface the repair reads.
    static constexpr std::string_view kSql = "SELECT * FROM conversations WHERE id = ?";
    std::vector<SqlRow> rows =
        sql_.query(std::string(kSql), {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    kotlin_check(rows.front().size() >= 2, "MaintenanceDebtDao: conversation row shape mismatch");
    ConversationEntry entry;
    entry.id = rows.front()[0].as_text();
    entry.selected_run_branches_json = rows.front()[1].text_or_null();
    return entry;
}

std::vector<RunEntry> MaintenanceDebtDao::get_maintenance_runs(std::string_view conversation_id) {
    static constexpr std::string_view kSql = "SELECT * FROM runs WHERE conversationId = ?";
    std::vector<RunEntry> out;
    for (const SqlRow& row : sql_.query(std::string(kSql),
                                        {SqlValue::text_value(std::string(conversation_id))})) {
        out.push_back(run_from_row(row));
    }
    return out;
}

std::int64_t MaintenanceDebtDao::compare_and_set_maintenance_run_branches(
    std::string_view conversation_id, std::string_view expected, std::string_view replacement,
    std::int64_t at) {
    static constexpr std::string_view kSql =
        "UPDATE conversations SET selectedRunBranchesJson = ?, "
        "dataChangedAt = MAX(dataChangedAt + 1, ?) "
        "WHERE id = ? AND selectedRunBranchesJson = ?";
    SqlArgs args;
    args.push_back(SqlValue::text_value(std::string(replacement)));
    args.push_back(SqlValue::integer_value(at));
    args.push_back(SqlValue::text_value(std::string(conversation_id)));
    args.push_back(SqlValue::text_value(std::string(expected)));
    return sql_.execute(std::string(kSql), args);
}

std::optional<DebtEntry> MaintenanceDebtDao::get_debt(std::string_view kind,
                                                     std::string_view identity) {
    const std::string sql = std::string("SELECT ") + std::string(kDebtColumns) +
                            " FROM maintenance_debt WHERE kind = ? AND identity = ?";
    std::vector<SqlRow> rows = sql_.query(sql, {SqlValue::text_value(std::string(kind)),
                                                SqlValue::text_value(std::string(identity))});
    if (rows.empty()) return std::nullopt;
    return debt_from_row(rows.front());
}

std::int64_t MaintenanceDebtDao::complete_claimed(std::string_view kind, std::string_view identity,
                                                  std::int64_t revision,
                                                  std::string_view claim_id) {
    static constexpr std::string_view kSql =
        "DELETE FROM maintenance_debt WHERE kind = ? AND identity = ? AND state = 'CLAIMED' "
        "AND revision = ? AND claimId = ?";
    SqlArgs args;
    args.push_back(SqlValue::text_value(std::string(kind)));
    args.push_back(SqlValue::text_value(std::string(identity)));
    args.push_back(SqlValue::integer_value(revision));
    args.push_back(SqlValue::text_value(std::string(claim_id)));
    return sql_.execute(std::string(kSql), args);
}

std::int64_t MaintenanceDebtDao::release_claimed(std::string_view kind, std::string_view identity,
                                                 std::int64_t revision, std::string_view claim_id,
                                                 std::int64_t updated_at) {
    static constexpr std::string_view kSql =
        "UPDATE maintenance_debt SET state = 'PENDING', updatedAt = ?, claimId = NULL, "
        "claimedAt = NULL WHERE kind = ? AND identity = ? AND state = 'CLAIMED' "
        "AND revision = ? AND claimId = ?";
    SqlArgs args;
    args.push_back(SqlValue::integer_value(updated_at));
    args.push_back(SqlValue::text_value(std::string(kind)));
    args.push_back(SqlValue::text_value(std::string(identity)));
    args.push_back(SqlValue::integer_value(revision));
    args.push_back(SqlValue::text_value(std::string(claim_id)));
    return sql_.execute(std::string(kSql), args);
}

// ---- @Transaction default methods -----------------------------------------------------------

DebtEntry MaintenanceDebtDao::enqueue(std::string_view kind, std::string_view identity,
                                      std::int64_t updated_at) {
    return run_in_transaction(*this, [&] {
        kotlin_require(!is_blank(kind));
        kotlin_require(!is_blank(identity));
        DebtEntry entry;
        entry.kind = std::string(kind);
        entry.identity = std::string(identity);
        entry.updated_at = updated_at;
        entry.validate();
        const std::int64_t inserted = insert_debt(entry);
        if (inserted == kInsertIgnored) {
            kotlin_check(renew_debt(kind, identity, updated_at) == 1,
                         "Maintenance debt disappeared during enqueue");
        }
        std::optional<DebtEntry> debt = get_debt(kind, identity);
        kotlin_check(debt.has_value(), "Maintenance debt was not readable after enqueue");
        return *debt;
    });
}

std::vector<DebtEntry> MaintenanceDebtDao::claim(std::string_view claim_id,
                                                 std::int64_t claimed_at,
                                                 std::int64_t stale_before, std::int32_t limit) {
    return run_in_transaction(*this, [&] {
        kotlin_require(!is_blank(claim_id));
        kotlin_require(claimed_at >= stale_before);
        kotlin_require(limit >= 1 && limit <= kMaxClaimBatch);
        std::vector<DebtEntry> claimed;
        for (const DebtEntry& candidate : get_claim_candidates(stale_before, limit)) {
            if (claim_candidate(candidate.kind, candidate.identity, candidate.revision, claim_id,
                                claimed_at, stale_before) == 1) {
                std::optional<DebtEntry> debt = get_debt(candidate.kind, candidate.identity);
                kotlin_check(debt.has_value(), "Claimed maintenance debt disappeared");
                kotlin_check(debt->revision == candidate.revision && debt->claim_id == claim_id &&
                                 debt->claimed_at == claimed_at,
                             "Maintenance debt claim changed inside its transaction");
                claimed.push_back(*debt);
            }
        }
        return claimed;
    });
}

bool MaintenanceDebtDao::complete(const DebtEntry& claim) {
    return run_in_transaction(*this, [&] {
        kotlin_require(claim.state == kStateClaimed);
        kotlin_require(claim.claim_id.has_value());
        kotlin_require(claim.claimed_at.has_value());
        return complete_claimed(claim.kind, claim.identity, claim.revision, *claim.claim_id) == 1;
    });
}

bool MaintenanceDebtDao::release(const DebtEntry& claim, std::int64_t updated_at) {
    return run_in_transaction(*this, [&] {
        kotlin_require(claim.state == kStateClaimed);
        kotlin_require(claim.claim_id.has_value());
        kotlin_require(claim.claimed_at.has_value());
        return release_claimed(claim.kind, claim.identity, claim.revision, *claim.claim_id,
                               updated_at) == 1;
    });
}

bool MaintenanceDebtDao::repair_run_branches(std::string_view conversation_id,
                                             std::int64_t now_ms) {
    return run_in_transaction(*this, [&] {
        kotlin_require(!is_blank(conversation_id));
        std::optional<ConversationEntry> conversation =
            get_maintenance_conversation(conversation_id);
        if (!conversation.has_value()) return false;
        if (!conversation->selected_run_branches_json.has_value()) return false;
        const std::string& raw = *conversation->selected_run_branches_json;
        const std::optional<RunSelections> decoded = run_branch::decode_selection_map(raw);
        RunSelections repaired;
        if (decoded.has_value()) {
            std::vector<run_branch::RunRef> runs;
            for (const RunEntry& run : get_maintenance_runs(conversation_id)) {
                runs.push_back(run_branch::RunRef{run.id, run.parent_run_id});
            }
            std::vector<run_branch::Selection> selections;
            selections.reserve(decoded->size());
            for (const auto& [parent, child] : *decoded) {
                selections.push_back(run_branch::Selection{parent, child});
            }
            for (const run_branch::Selection& kept : retain_valid_edges(selections, runs)) {
                repaired.emplace_back(kept.parent_run_id, kept.child_run_id);
            }
        }
        if (decoded.has_value() && repaired == *decoded) return false;
        return compare_and_set_maintenance_run_branches(conversation_id, raw,
                                                        run_branch::encode_selection_map(repaired),
                                                        now_ms) == 1;
    });
}

}  // namespace agora::core::maintenance_debt
