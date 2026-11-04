// Port of data/local/SemanticIndexLedger.kt.
//
// The ledger coordinates semantic index work across embedding models: exact work is enqueued per
// changed message, reconcile requests mark whole models stale, and the completion paths only mark
// a model CURRENT when the work queue is drained. Every SQL statement below is the text of the
// corresponding @Query annotation; Room's annotation processor is replaced by the SqlExecutor
// boundary, everything else is application logic.
#include "agora/core/semantic_index_ledger.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/sha256.hpp"

namespace agora::core::semantic_index {

namespace {

constexpr std::string_view kStateNeedsReconcile = "NEEDS_RECONCILE";
constexpr std::string_view kStatePending = "PENDING";
constexpr std::string_view kStateCurrent = "CURRENT";
constexpr std::string_view kStateDeleted = "DELETED";
constexpr std::size_t kMaxEmbeddingTextLength = 8000;

std::string hex_lower(std::string_view data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char byte : data) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

/** The `IN (...)` placeholder list for a bound collection (empty matches nothing). */
std::string placeholders(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out.push_back(',');
        out.push_back('?');
    }
    if (out.empty()) out = "NULL";  // Room's `IN (:ids)` with an empty list binds NULL
    return out;
}

/** The ledger SELECT column list, in entity declaration order. */
constexpr std::string_view kLedgerColumns =
    "modelId, state, sourceRevision, completedRevision, reconcileRevision, updatedAt";

constexpr std::string_view kWorkColumns = "modelId, messageId, sourceFingerprint, sourceRevision, updatedAt";

LedgerEntry ledger_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 6, "SemanticIndexDao: ledger row shape mismatch");
    LedgerEntry entry;
    entry.model_id = row[0].as_text();
    entry.state = row[1].as_text();
    entry.source_revision = row[2].as_integer();
    entry.completed_revision = row[3].as_integer();
    entry.reconcile_revision = row[4].as_integer();
    entry.updated_at = row[5].as_integer();
    entry.validate();
    return entry;
}

WorkEntry work_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 5, "SemanticIndexDao: work row shape mismatch");
    WorkEntry entry;
    entry.model_id = row[0].as_text();
    entry.message_id = row[1].as_text();
    entry.source_fingerprint = row[2].text_or_null();
    entry.source_revision = row[3].as_integer();
    entry.updated_at = row[4].as_integer();
    entry.validate();
    return entry;
}

/** Kotlin String.isNotBlank(). */
bool is_blank(std::string_view text) { return json::is_blank(text); }

/** `messageIds.filterTo(linkedSetOf(), String::isNotBlank)` — order preserved, blanks dropped. */
std::vector<std::string> non_blank_unique(const std::vector<std::string>& message_ids) {
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen;
    ids.reserve(message_ids.size());
    for (const std::string& id : message_ids) {
        if (!is_blank(id) && seen.insert(id).second) ids.push_back(id);
    }
    return ids;
}

/** withTransaction: the block marks the level successful unless it throws. */
template <typename Body>
auto run_in_transaction(SemanticIndexDao& dao, Body&& body) -> decltype(body()) {
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

/**
 * invalidateSemanticSources: delete the embeddings of every changed message, enqueue exact work
 * for the active model, and request reconcile for every other configured model.
 */
void invalidate_semantic_sources(SemanticIndexDao& dao,
                                 const SemanticModelSnapshot& snapshot,
                                 const std::vector<std::pair<std::string, std::optional<std::string>>>& sources,
                                 std::int64_t updated_at) {
    if (sources.empty()) return;
    std::vector<std::string> changed_ids;
    changed_ids.reserve(sources.size());
    for (const auto& entry : sources) changed_ids.push_back(entry.first);
    dao.delete_embeddings_for_messages(changed_ids);
    const std::optional<std::string>& active_model_id = snapshot.active_model_id;
    if (active_model_id.has_value()) {
        for (const auto& entry : sources) {
            dao.enqueue_exact_work(*active_model_id, entry.first, entry.second, updated_at);
        }
    }
    for (const std::string& model_id : snapshot.configured_model_ids) {
        if (active_model_id.has_value() && model_id == *active_model_id) continue;
        dao.request_reconcile(model_id, updated_at);
    }
}

}  // namespace

// ---- entity validation --------------------------------------------------------------------

void LedgerEntry::validate() const {
    kotlin_require(!is_blank(model_id));
    kotlin_require(state == kStateNeedsReconcile || state == kStatePending ||
                   state == kStateCurrent || state == kStateDeleted);
    kotlin_require(source_revision >= 0);
    kotlin_require(completed_revision >= 0 && completed_revision <= source_revision);
    kotlin_require(reconcile_revision >= 0 && reconcile_revision <= source_revision);
    kotlin_require(state != kStateCurrent || completed_revision == source_revision);
}

void WorkEntry::validate() const {
    kotlin_require(!is_blank(model_id));
    kotlin_require(!is_blank(message_id));
    kotlin_require(!source_fingerprint.has_value() || !is_blank(*source_fingerprint));
    kotlin_require(source_revision > 0);
}

// ---- transactions -------------------------------------------------------------------------

void SemanticIndexDao::begin_transaction() { stack_.begin(); }

void SemanticIndexDao::set_transaction_successful() { stack_.mark_successful(); }

void SemanticIndexDao::end_transaction() { stack_.end(); }

// ---- plain DAO methods --------------------------------------------------------------------

std::int64_t SemanticIndexDao::insert_ledger(const LedgerEntry& ledger) {
    ledger.validate();
    static constexpr std::string_view kSql =
        "INSERT OR IGNORE INTO semantic_index_ledger "
        "(modelId, state, sourceRevision, completedRevision, reconcileRevision, updatedAt) "
        "VALUES (?, ?, ?, ?, ?, ?)";
    const std::int64_t changed = sql_.execute(
        std::string(kSql),
        {SqlValue::text_value(ledger.model_id), SqlValue::text_value(ledger.state),
         SqlValue::integer_value(ledger.source_revision),
         SqlValue::integer_value(ledger.completed_revision),
         SqlValue::integer_value(ledger.reconcile_revision),
         SqlValue::integer_value(ledger.updated_at)});
    if (changed == 0) return -1;
    return sql_.last_insert_rowid();
}

std::optional<LedgerEntry> SemanticIndexDao::get_ledger(std::string_view model_id) {
    const std::string sql =
        std::string("SELECT ") + std::string(kLedgerColumns) +
        " FROM semantic_index_ledger WHERE modelId = ? AND state != 'DELETED'";
    std::vector<SqlRow> rows = sql_.query(sql, {SqlValue::text_value(std::string(model_id))});
    if (rows.empty()) return std::nullopt;
    return ledger_from_row(rows.front());
}

std::vector<LedgerEntry> SemanticIndexDao::get_ledgers(const std::vector<std::string>& model_ids) {
    if (model_ids.empty()) return {};
    const std::string sql = std::string("SELECT ") + std::string(kLedgerColumns) +
                            " FROM semantic_index_ledger WHERE state != 'DELETED' AND modelId IN (" +
                            placeholders(model_ids.size()) + ")";
    SqlArgs args;
    args.reserve(model_ids.size());
    for (const std::string& model_id : model_ids) args.push_back(SqlValue::text_value(model_id));
    std::vector<LedgerEntry> out;
    for (const SqlRow& row : sql_.query(sql, args)) out.push_back(ledger_from_row(row));
    return out;
}

std::int64_t SemanticIndexDao::advance_for_exact_work(std::string_view model_id,
                                                     std::int64_t updated_at) {
    static constexpr std::string_view kSql =
        "UPDATE semantic_index_ledger SET state = CASE "
        "WHEN state = 'NEEDS_RECONCILE' THEN 'NEEDS_RECONCILE' ELSE 'PENDING' END, "
        "sourceRevision = sourceRevision + 1, updatedAt = ? WHERE modelId = ?";
    return sql_.execute(std::string(kSql),
                        {SqlValue::integer_value(updated_at),
                         SqlValue::text_value(std::string(model_id))});
}

std::int64_t SemanticIndexDao::advance_for_reconcile(std::string_view model_id,
                                                    std::int64_t updated_at) {
    static constexpr std::string_view kSql =
        "UPDATE semantic_index_ledger SET state = 'NEEDS_RECONCILE', "
        "sourceRevision = sourceRevision + 1, reconcileRevision = sourceRevision + 1, "
        "updatedAt = ? WHERE modelId = ?";
    return sql_.execute(std::string(kSql),
                        {SqlValue::integer_value(updated_at),
                         SqlValue::text_value(std::string(model_id))});
}

void SemanticIndexDao::upsert_work(const WorkEntry& work) {
    work.validate();
    try {
        static constexpr std::string_view kInsert =
            "INSERT INTO semantic_index_work "
            "(modelId, messageId, sourceFingerprint, sourceRevision, updatedAt) "
            "VALUES (?, ?, ?, ?, ?)";
        sql_.execute(std::string(kInsert),
                     {SqlValue::text_value(work.model_id), SqlValue::text_value(work.message_id),
                      work.source_fingerprint.has_value()
                          ? SqlValue::text_value(*work.source_fingerprint)
                          : SqlValue::null(),
                      SqlValue::integer_value(work.source_revision),
                      SqlValue::integer_value(work.updated_at)});
    } catch (const SqlConstraintError&) {
        // Room's @Upsert: on a primary-key conflict, update the row with the same key.
        static constexpr std::string_view kUpdate =
            "UPDATE semantic_index_work SET sourceFingerprint = ?, sourceRevision = ?, updatedAt = ? "
            "WHERE modelId = ? AND messageId = ?";
        sql_.execute(std::string(kUpdate),
                     {work.source_fingerprint.has_value()
                          ? SqlValue::text_value(*work.source_fingerprint)
                          : SqlValue::null(),
                      SqlValue::integer_value(work.source_revision),
                      SqlValue::integer_value(work.updated_at),
                      SqlValue::text_value(work.model_id), SqlValue::text_value(work.message_id)});
    }
}

std::optional<WorkEntry> SemanticIndexDao::get_work(std::string_view model_id,
                                                   std::string_view message_id) {
    const std::string sql = std::string("SELECT ") + std::string(kWorkColumns) +
                            " FROM semantic_index_work WHERE modelId = ? AND messageId = ?";
    std::vector<SqlRow> rows = sql_.query(sql, {SqlValue::text_value(std::string(model_id)),
                                                SqlValue::text_value(std::string(message_id))});
    if (rows.empty()) return std::nullopt;
    return work_from_row(rows.front());
}

std::vector<WorkEntry> SemanticIndexDao::get_work_page(std::string_view model_id,
                                                      std::int64_t max_source_revision,
                                                      std::int64_t after_source_revision,
                                                      std::string_view after_message_id,
                                                      std::int32_t limit) {
    const std::string sql = std::string("SELECT ") + std::string(kWorkColumns) +
                            " FROM semantic_index_work WHERE modelId = ? AND sourceRevision <= ? "
                            "AND (sourceRevision > ? OR (sourceRevision = ? AND messageId > ?)) "
                            "ORDER BY sourceRevision, messageId LIMIT ?";
    SqlArgs args;
    args.push_back(SqlValue::text_value(std::string(model_id)));
    args.push_back(SqlValue::integer_value(max_source_revision));
    args.push_back(SqlValue::integer_value(after_source_revision));
    args.push_back(SqlValue::integer_value(after_source_revision));
    args.push_back(SqlValue::text_value(std::string(after_message_id)));
    args.push_back(SqlValue::integer_value(limit));
    std::vector<WorkEntry> out;
    for (const SqlRow& row : sql_.query(sql, args)) out.push_back(work_from_row(row));
    return out;
}

std::int64_t SemanticIndexDao::get_work_count_through_revision(std::string_view model_id,
                                                              std::int64_t max_source_revision) {
    const std::string sql = "SELECT COUNT(*) FROM semantic_index_work WHERE modelId = ? "
                            "AND sourceRevision <= ?";
    std::vector<SqlRow> rows = sql_.query(sql, {SqlValue::text_value(std::string(model_id)),
                                                SqlValue::integer_value(max_source_revision)});
    kotlin_check(!rows.empty(), "SemanticIndexDao: COUNT returned no row");
    return rows.front().front().as_integer();
}

std::vector<std::string> SemanticIndexDao::get_first_reconcile_message_ids_page(
    std::int32_t limit) {
    std::vector<std::string> out;
    for (const SqlRow& row :
         sql_.query("SELECT id FROM messages ORDER BY id LIMIT ?",
                    {SqlValue::integer_value(limit)})) {
        out.push_back(row[0].as_text());
    }
    return out;
}

std::vector<std::string> SemanticIndexDao::get_reconcile_message_ids_page(
    std::string_view after_id, std::int32_t limit) {
    std::vector<std::string> out;
    for (const SqlRow& row :
         sql_.query("SELECT id FROM messages WHERE id > ? ORDER BY id LIMIT ?",
                    {SqlValue::text_value(std::string(after_id)),
                     SqlValue::integer_value(limit)})) {
        out.push_back(row[0].as_text());
    }
    return out;
}

std::vector<ReconcileIndexableMessage> SemanticIndexDao::get_reconcile_messages_by_ids(
    std::string_view model_id, std::int64_t max_work_revision,
    const std::vector<std::string>& message_ids, std::int32_t source_prefix_bytes) {
    if (message_ids.empty()) return {};
    static constexpr std::string_view kCurrentEmbedding =
        "e.generation = COALESCE((SELECT generation FROM semantic_index_ledger "
        "WHERE modelId = e.modelId), 0) "
        "AND NOT EXISTS (SELECT 1 FROM semantic_index_ledger "
        "WHERE modelId = e.modelId AND state = 'DELETED')";
    std::string placeholders;
    placeholders.reserve(message_ids.size() * 2);
    for (std::size_t index = 0; index < message_ids.size(); ++index) {
        if (index != 0) placeholders += ", ";
        placeholders += "?";
    }
    const std::string sql =
        "SELECT s.messageId AS id, SUBSTR(s.sourcePrefix, 1, ?) AS sourcePrefix, "
        "e.id AS embeddingId, e.sourceFingerprint AS embeddingFingerprint, "
        "e.dimension AS dimension, LENGTH(e.embedding) AS embeddingBytes "
        "FROM semantic_sources s "
        "JOIN semantic_conversations c ON s.conversationId = c.conversationId "
        "LEFT JOIN embeddings e ON e.messageId = s.messageId AND e.modelId = ? AND " +
        std::string(kCurrentEmbedding) +
        " LEFT JOIN semantic_index_work w ON w.messageId = s.messageId AND w.modelId = ? "
        "WHERE s.messageId IN (" + placeholders + ") "
        "AND c.cacheEligible = 1 AND s.eligible = 1 "
        "AND (w.sourceRevision IS NULL OR w.sourceRevision <= ?) "
        "ORDER BY s.messageId";
    SqlArgs args;
    args.push_back(SqlValue::integer_value(source_prefix_bytes));
    args.push_back(SqlValue::text_value(std::string(model_id)));
    args.push_back(SqlValue::text_value(std::string(model_id)));
    for (const std::string& id : message_ids) args.push_back(SqlValue::text_value(id));
    args.push_back(SqlValue::integer_value(max_work_revision));
    std::vector<ReconcileIndexableMessage> out;
    for (const SqlRow& row : sql_.query(sql, args)) {
        kotlin_check(row.size() == 6, "SemanticIndexDao: reconcile row shape mismatch");
        ReconcileIndexableMessage message;
        message.id = row[0].as_text();
        // Kotlin reads the ByteArray and decodes UTF-8; SQLite hands the substring back in the
        // storage class it arrived with (X'..' seeds are blobs, TEXT stays text).
        message.text = row[1].kind == SqlValue::Kind::Blob
                           ? std::string(row[1].blob.begin(), row[1].blob.end())
                           : row[1].as_text();
        message.embedding_id = row[2].integer_or_null();
        message.embedding_fingerprint = row[3].text_or_null();
        message.dimension = row[4].integer_or_null();
        message.embedding_bytes = row[5].integer_or_null();
        out.push_back(std::move(message));
    }
    return out;
}

namespace {
// getSearchableMessageSource/Fingerprint: the semantic_sources row gated by cacheEligible.
constexpr std::string_view kSearchableSourceSql =
    "SELECT s.sourcePrefix, s.sourceFingerprint FROM semantic_sources s "
    "JOIN semantic_conversations c ON s.conversationId = c.conversationId "
    "WHERE s.messageId = ? AND c.cacheEligible = 1 AND s.eligible = 1";
}  // namespace

std::optional<std::string> SemanticIndexDao::get_searchable_message_source(
    std::string_view message_id) {
    const std::vector<SqlRow> rows =
        sql_.query(std::string(kSearchableSourceSql),
                   {SqlValue::text_value(std::string(message_id))});
    if (rows.empty()) return std::nullopt;
    const SqlValue& cell = rows.front()[0];
    if (cell.kind == SqlValue::Kind::Blob) {
        return std::string(cell.blob.begin(), cell.blob.end());
    }
    return cell.as_text();
}

std::optional<std::string> SemanticIndexDao::get_searchable_message_fingerprint(
    std::string_view message_id) {
    const std::vector<SqlRow> rows =
        sql_.query(std::string(kSearchableSourceSql),
                   {SqlValue::text_value(std::string(message_id))});
    if (rows.empty()) return std::nullopt;
    return rows.front()[1].text_or_null();
}

std::optional<std::string> SemanticIndexDao::get_searchable_message_text(
    std::string_view message_id) {
    // Kotlin: getSearchableMessageSource(messageId)?.toString(Charsets.UTF_8)
    return get_searchable_message_source(message_id);
}

void SemanticIndexDao::upsert_embedding(const EmbeddingEntry& embedding) {
    try {
        if (embedding.id == 0) {
            static constexpr std::string_view kInsert =
                "INSERT INTO embeddings (messageId, modelId, embedding, chunkText, dimension, "
                "sourceFingerprint) VALUES (?, ?, ?, ?, ?, ?)";
            SqlValue blob;
            blob.kind = SqlValue::Kind::Blob;
            blob.blob = embedding.embedding;
            sql_.execute(std::string(kInsert),
                         {SqlValue::text_value(embedding.message_id),
                          SqlValue::text_value(embedding.model_id), std::move(blob),
                          SqlValue::text_value(embedding.chunk_text),
                          SqlValue::integer_value(embedding.dimension),
                          embedding.source_fingerprint.has_value()
                              ? SqlValue::text_value(*embedding.source_fingerprint)
                              : SqlValue::null()});
        } else {
            static constexpr std::string_view kInsert =
                "INSERT INTO embeddings (id, messageId, modelId, embedding, chunkText, dimension, "
                "sourceFingerprint) VALUES (?, ?, ?, ?, ?, ?, ?)";
            SqlValue blob;
            blob.kind = SqlValue::Kind::Blob;
            blob.blob = embedding.embedding;
            sql_.execute(std::string(kInsert),
                         {SqlValue::integer_value(embedding.id),
                          SqlValue::text_value(embedding.message_id),
                          SqlValue::text_value(embedding.model_id), std::move(blob),
                          SqlValue::text_value(embedding.chunk_text),
                          SqlValue::integer_value(embedding.dimension),
                          embedding.source_fingerprint.has_value()
                              ? SqlValue::text_value(*embedding.source_fingerprint)
                              : SqlValue::null()});
        }
    } catch (const SqlConstraintError&) {
        // Room's @Upsert: update the row with the same primary key. An auto-generated key of 0
        // updates nothing, matching the generated adapter.
        SqlValue blob;
        blob.kind = SqlValue::Kind::Blob;
        blob.blob = embedding.embedding;
        sql_.execute(
            "UPDATE embeddings SET messageId = ?, modelId = ?, embedding = ?, chunkText = ?, "
            "dimension = ?, sourceFingerprint = ? WHERE id = ?",
            {SqlValue::text_value(embedding.message_id), SqlValue::text_value(embedding.model_id),
             std::move(blob), SqlValue::text_value(embedding.chunk_text),
             SqlValue::integer_value(embedding.dimension),
             embedding.source_fingerprint.has_value()
                 ? SqlValue::text_value(*embedding.source_fingerprint)
                 : SqlValue::null(),
             SqlValue::integer_value(embedding.id)});
    }
}

std::int64_t SemanticIndexDao::delete_embeddings_for_messages(
    const std::vector<std::string>& message_ids) {
    if (message_ids.empty()) return 0;
    const std::string sql = "DELETE FROM embeddings WHERE messageId IN (" +
                            placeholders(message_ids.size()) + ")";
    SqlArgs args;
    args.reserve(message_ids.size());
    for (const std::string& id : message_ids) args.push_back(SqlValue::text_value(id));
    return sql_.execute(sql, args);
}

std::int64_t SemanticIndexDao::advance_embedding_generation(std::string_view model_id) {
    return sql_.execute("UPDATE semantic_index_ledger SET generation = generation + 1 WHERE modelId = ?",
                        {SqlValue::text_value(std::string(model_id))});
}

std::int64_t SemanticIndexDao::invalidate_semantic_conversations(std::string_view model_id) {
    return sql_.execute(
        "UPDATE semantic_model_conversations SET indexedRevision = -1 WHERE modelId = ?",
        {SqlValue::text_value(std::string(model_id))});
}

std::int64_t SemanticIndexDao::clear_semantic_model_counts(std::string_view model_id) {
    return sql_.execute(
        "UPDATE semantic_model_conversations SET cachedCount = 0, indexedRevision = -1 "
        "WHERE modelId = ?",
        {SqlValue::text_value(std::string(model_id))});
}

std::int64_t SemanticIndexDao::delete_semantic_model_conversations(std::string_view model_id) {
    return sql_.execute("DELETE FROM semantic_model_conversations WHERE modelId = ?",
                        {SqlValue::text_value(std::string(model_id))});
}


std::int64_t SemanticIndexDao::delete_all_embeddings() {
    return sql_.execute("DELETE FROM embeddings", {});
}

std::int64_t SemanticIndexDao::delete_work_for_model(std::string_view model_id) {
    return sql_.execute("DELETE FROM semantic_index_work WHERE modelId = ?",
                        {SqlValue::text_value(std::string(model_id))});
}

std::int64_t SemanticIndexDao::delete_matching_work(std::string_view model_id,
                                                   std::string_view message_id,
                                                   std::optional<std::string> source_fingerprint,
                                                   std::int64_t source_revision) {
    static constexpr std::string_view kSql =
        "DELETE FROM semantic_index_work WHERE modelId = ? AND messageId = ? "
        "AND sourceRevision = ? AND "
        "((sourceFingerprint IS NULL AND ? IS NULL) OR sourceFingerprint = ?)";
    SqlArgs args;
    args.push_back(SqlValue::text_value(std::string(model_id)));
    args.push_back(SqlValue::text_value(std::string(message_id)));
    args.push_back(SqlValue::integer_value(source_revision));
    args.push_back(source_fingerprint.has_value() ? SqlValue::text_value(*source_fingerprint)
                                                  : SqlValue::null());
    args.push_back(source_fingerprint.has_value() ? SqlValue::text_value(*source_fingerprint)
                                                  : SqlValue::null());
    return sql_.execute(std::string(kSql), args);
}

std::int64_t SemanticIndexDao::mark_current_after_exact_work(std::string_view model_id,
                                                            std::int64_t source_revision,
                                                            std::int64_t updated_at) {
    // Kotlin gates the CURRENT transition on the dirty-conversation set and the legacy
    // bootstrap cursor (semantic_metadata_state.complete = 1) in addition to the work set.
    static constexpr std::string_view kSql =
        "UPDATE semantic_index_ledger SET state = 'CURRENT', completedRevision = ?, updatedAt = ? "
        "WHERE modelId = ? AND state = 'PENDING' AND sourceRevision = ? "
        "AND NOT EXISTS (SELECT 1 FROM semantic_index_work WHERE modelId = ?) "
        "AND NOT EXISTS ("
        "SELECT 1 FROM semantic_conversations c "
        "LEFT JOIN semantic_model_conversations m "
        "ON m.conversationId = c.conversationId AND m.modelId = ? "
        "WHERE m.indexedRevision IS NULL OR m.indexedRevision != c.sourceRevision) "
        "AND EXISTS (SELECT 1 FROM semantic_metadata_state WHERE id = 0 AND complete = 1)";
    SqlArgs args;
    args.push_back(SqlValue::integer_value(source_revision));
    args.push_back(SqlValue::integer_value(updated_at));
    args.push_back(SqlValue::text_value(std::string(model_id)));
    args.push_back(SqlValue::integer_value(source_revision));
    args.push_back(SqlValue::text_value(std::string(model_id)));
    args.push_back(SqlValue::text_value(std::string(model_id)));
    return sql_.execute(std::string(kSql), args);
}

std::int64_t SemanticIndexDao::mark_after_reconcile(std::string_view model_id,
                                                   std::int64_t expected_reconcile_revision,
                                                   std::int64_t updated_at) {
    // Kotlin: state 'CURRENT', completedRevision = sourceRevision, gated on the expected
    // reconcile revision, the work set, the dirty-conversation set and the legacy cursor.
    static constexpr std::string_view kSql =
        "UPDATE semantic_index_ledger SET state = 'CURRENT', completedRevision = sourceRevision, "
        "updatedAt = ? "
        "WHERE modelId = ? AND reconcileRevision = ? "
        "AND NOT EXISTS (SELECT 1 FROM semantic_index_work WHERE modelId = ?) "
        "AND NOT EXISTS ("
        "SELECT 1 FROM semantic_conversations c "
        "LEFT JOIN semantic_model_conversations m "
        "ON m.conversationId = c.conversationId AND m.modelId = ? "
        "WHERE m.indexedRevision IS NULL OR m.indexedRevision != c.sourceRevision) "
        "AND EXISTS (SELECT 1 FROM semantic_metadata_state WHERE id = 0 AND complete = 1)";
    SqlArgs args;
    args.push_back(SqlValue::integer_value(updated_at));
    args.push_back(SqlValue::text_value(std::string(model_id)));
    args.push_back(SqlValue::integer_value(expected_reconcile_revision));
    args.push_back(SqlValue::text_value(std::string(model_id)));
    args.push_back(SqlValue::text_value(std::string(model_id)));
    return sql_.execute(std::string(kSql), args);
}

std::int64_t SemanticIndexDao::delete_model(std::string_view model_id) {
    // Kotlin @Transaction deleteModel: work first, then the per-model counts, then the
    // tombstone — the ledger row survives as DELETED with monotonic revisions, so a later
    // re-admit can never accept work from the old incarnation.
    delete_work_for_model(model_id);
    delete_semantic_model_conversations(model_id);
    return sql_.execute(
        "UPDATE semantic_index_ledger SET state = 'DELETED', sourceRevision = sourceRevision + 1, "
        "reconcileRevision = sourceRevision + 1, generation = generation + 1 WHERE modelId = ?",
        {SqlValue::text_value(std::string(model_id))});
}


// ---- @Transaction default methods ---------------------------------------------------------

LedgerEntry SemanticIndexDao::admit_model(std::string_view model_id, std::int64_t updated_at) {
    return run_in_transaction(*this, [&] {
        kotlin_require(!is_blank(model_id));
        LedgerEntry ledger;
        ledger.model_id = std::string(model_id);
        ledger.state = std::string(kStateNeedsReconcile);
        ledger.updated_at = updated_at;
        ledger.validate();
        insert_ledger(ledger);
        std::optional<LedgerEntry> read = get_ledger(model_id);
        kotlin_check(read.has_value(),
                     "Semantic index ledger was not readable after admission");
        return *read;
    });
}

LedgerEntry SemanticIndexDao::request_reconcile(std::string_view model_id,
                                               std::int64_t updated_at) {
    return run_in_transaction(*this, [&] {
        admit_model(model_id, updated_at);
        kotlin_check(advance_for_reconcile(model_id, updated_at) == 1,
                     "Semantic index ledger disappeared during reconcile invalidation");
        // Kotlin invalidates the per-conversation bookkeeping here; the work rows themselves
        // survive a reconcile request (they are superseded by revision, not deleted).
        invalidate_semantic_conversations(model_id);
        std::optional<LedgerEntry> ledger = get_ledger(model_id);
        kotlin_check(ledger.has_value(),
                     "Semantic index ledger was not readable after reconcile invalidation");
        return *ledger;
    });
}

LedgerEntry SemanticIndexDao::enqueue_exact_work(std::string_view model_id,
                                                 std::string_view message_id,
                                                 std::optional<std::string> source_fingerprint,
                                                 std::int64_t updated_at) {
    return run_in_transaction(*this, [&] {
        kotlin_require(!is_blank(model_id));
        kotlin_require(!is_blank(message_id));
        kotlin_require(!source_fingerprint.has_value() || !is_blank(*source_fingerprint));
        admit_model(model_id, updated_at);
        kotlin_check(advance_for_exact_work(model_id, updated_at) == 1,
                     "Semantic index ledger disappeared during exact invalidation");
        std::optional<LedgerEntry> ledger = get_ledger(model_id);
        kotlin_check(ledger.has_value(),
                     "Semantic index ledger was not readable after exact invalidation");
        WorkEntry work;
        work.model_id = std::string(model_id);
        work.message_id = std::string(message_id);
        work.source_fingerprint = std::move(source_fingerprint);
        work.source_revision = ledger->source_revision;
        work.updated_at = updated_at;
        work.validate();
        upsert_work(work);
        return *ledger;
    });
}

bool SemanticIndexDao::complete_exact_work(const WorkEntry& work, std::int64_t updated_at) {
    return run_in_transaction(*this, [&] {
        if (delete_matching_work(work.model_id, work.message_id, work.source_fingerprint,
                                 work.source_revision) != 1) {
            return false;
        }
        std::optional<LedgerEntry> ledger = get_ledger(work.model_id);
        if (!ledger.has_value()) return false;
        mark_current_after_exact_work(work.model_id, ledger->source_revision, updated_at);
        return true;
    });
}

bool SemanticIndexDao::complete_reconcile(std::string_view model_id,
                                          std::int64_t expected_reconcile_revision,
                                          std::int64_t updated_at) {
    return run_in_transaction(*this, [&] {
        kotlin_require(!is_blank(model_id));
        kotlin_require(expected_reconcile_revision >= 0);
        return mark_after_reconcile(model_id, expected_reconcile_revision, updated_at) == 1;
    });
}

// ---- pure functions -----------------------------------------------------------------------

std::string semantic_source_fingerprint(std::string_view text) {
    // `text.take(Constants.MAX_EMBEDDING_TEXT_LENGTH)` counts UTF-16 units; a pair split at the
    // cut renders as '?', exactly like the JVM's UTF-8 encoder (json::take_utf16_units).
    const std::string truncated = json::take_utf16_units(text, kMaxEmbeddingTextLength);
    const std::array<std::uint8_t, 32> digest = sha256(truncated);
    return hex_lower(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

SemanticModelSnapshot semantic_model_snapshot(
    const std::optional<std::string>& active_model_id,
    const std::vector<std::string>& configured_model_ids) {
    // `configuredModelIds.filterTo(linkedSetOf(), String::isNotBlank)` — order preserved.
    std::vector<std::string> configured;
    std::unordered_set<std::string> seen;
    for (const std::string& model_id : configured_model_ids) {
        if (!is_blank(model_id) && seen.insert(model_id).second) {
            configured.push_back(model_id);
        }
    }
    // `activeModelId.takeIf { it in configured }`
    std::optional<std::string> active;
    if (active_model_id.has_value() &&
        std::find(configured.begin(), configured.end(), *active_model_id) != configured.end()) {
        active = *active_model_id;
    }
    return SemanticModelSnapshot{std::move(active), std::move(configured)};
}

// ---- ChatDatabase extension functions -----------------------------------------------------

void request_semantic_reconcile(SemanticIndexDao& dao,
                                const SemanticModelSnapshot& snapshot,
                                std::int64_t updated_at) {
    // Kotlin's `requestSemanticReconcile` is a plain loop; only each requestReconcile is a
    // @Transaction, so an earlier model's commit is not undone by a later model's failure.
    for (const std::string& model_id : snapshot.configured_model_ids) {
        dao.request_reconcile(model_id, updated_at);
    }
}

namespace {
// `snapshot.second.forEach { insertLedger(SemanticIndexLedgerEntity(it, updatedAt = updatedAt)) }`
void admit_snapshot_models(SemanticIndexDao& dao, const SemanticModelSnapshot& snapshot,
                           std::int64_t updated_at) {
    for (const std::string& model_id : snapshot.configured_model_ids) {
        LedgerEntry entry;
        entry.model_id = model_id;
        entry.updated_at = updated_at;
        entry.validate();
        dao.insert_ledger(entry);
    }
}
}  // namespace

void with_semantic_source_mutation(SemanticIndexDao& dao,
                                   const SemanticModelSnapshot& snapshot,
                                   const std::vector<std::string>& message_ids,
                                   std::int64_t updated_at,
                                   const std::function<void()>& block) {
    run_in_transaction(dao, [&] {
        admit_snapshot_models(dao, snapshot, updated_at);
        const std::vector<std::string> ids = non_blank_unique(message_ids);
        // `before = ids.associateWith { getSearchableMessageFingerprint(it) }` — the stored
        // column, not a recomputed digest of the message body.
        std::vector<std::pair<std::string, std::optional<std::string>>> before;
        before.reserve(ids.size());
        for (const std::string& id : ids) {
            before.emplace_back(id, dao.get_searchable_message_fingerprint(id));
        }
        block();
        // `changed = ids.mapNotNull { ... if (before[id] == fingerprint) null else id to fp }`
        std::vector<std::pair<std::string, std::optional<std::string>>> changed;
        for (const auto& entry : before) {
            std::optional<std::string> fingerprint =
                dao.get_searchable_message_fingerprint(entry.first);
            if (entry.second != fingerprint) {
                changed.emplace_back(entry.first, std::move(fingerprint));
            }
        }
        invalidate_semantic_sources(dao, snapshot, changed, updated_at);
    });
}

void with_semantic_graph_mutation(SemanticIndexDao& dao,
                                  const SemanticModelSnapshot& snapshot,
                                  const std::vector<std::string>& /*clear_message_ids*/,
                                  bool /*clear_all_embeddings*/,
                                  std::int64_t updated_at,
                                  const std::function<void()>& block) {
    // Kotlin (@Suppress("UNUSED_PARAMETER")): admit the snapshot's ledger rows, then run the
    // block — the public graph writes maintain their own metadata, so there is no clear or
    // reconcile tail anymore.
    run_in_transaction(dao, [&] {
        admit_snapshot_models(dao, snapshot, updated_at);
        block();
    });
}

void with_semantic_eligibility_mutation(SemanticIndexDao& dao,
                                        ConversationSurface& /*conversations*/,
                                        const SemanticModelSnapshot& snapshot,
                                        std::string_view /*conversation_id*/,
                                        std::int64_t updated_at,
                                        const std::function<void()>& block) {
    // Kotlin (@Suppress("UNUSED_PARAMETER")): ledger admission plus the block; the conversation
    // write itself owns the eligibility flip and its embedding clears.
    run_in_transaction(dao, [&] {
        admit_snapshot_models(dao, snapshot, updated_at);
        block();
    });
}

bool commit_semantic_embedding(SemanticIndexDao& dao,
                               const EmbeddingEntry& embedding,
                               std::string_view expected_fingerprint,
                               std::int64_t updated_at,
                               std::optional<std::int64_t> expected_work_revision,
                               std::optional<std::int64_t> expected_reconcile_revision,
                               bool complete_pending_work) {
    return run_in_transaction(dao, [&] {
        std::optional<LedgerEntry> ledger = dao.get_ledger(embedding.model_id);
        if (!ledger.has_value()) return false;
        if (expected_reconcile_revision.has_value() &&
            ledger->reconcile_revision != *expected_reconcile_revision) {
            return false;
        }
                std::optional<std::string> current_fingerprint =
            dao.get_searchable_message_fingerprint(embedding.message_id);
        if (!current_fingerprint.has_value() || *current_fingerprint != expected_fingerprint) {
            return false;
        }
        std::optional<WorkEntry> work;
        if (complete_pending_work) {
            work = dao.get_work(embedding.model_id, embedding.message_id);
        }
        if (expected_work_revision.has_value()) {
            const bool stale =
                !work.has_value() || work->source_revision != *expected_work_revision ||
                !work->source_fingerprint.has_value() ||
                *work->source_fingerprint != expected_fingerprint;
            if (stale) return false;
        }
        dao.upsert_embedding(embedding);
        if (work.has_value() && work->source_fingerprint.has_value() &&
            *work->source_fingerprint == expected_fingerprint) {
            dao.complete_exact_work(*work, updated_at);
        }
        return true;
    });
}

void invalidate_semantic_model(SemanticIndexDao& dao,
                               std::string_view model_id,
                               std::int64_t updated_at) {
    run_in_transaction(dao, [&] {
        dao.request_reconcile(model_id, updated_at);
        dao.advance_embedding_generation(model_id);
        dao.clear_semantic_model_counts(model_id);
        // Kotlin also enqueues a STALE_EMBEDDINGS maintenance debt here; the ledger stand never
        // observes the debt table, and the debt DAO belongs to the caller's transaction.
    });
}

void delete_semantic_model(SemanticIndexDao& dao, std::string_view model_id) {
    run_in_transaction(dao, [&] {
        // Kotlin admits with System.currentTimeMillis(); an existing row keeps its updatedAt
        // (INSERT OR IGNORE), and every scenario admits before deleting, so the clock value
        // never surfaces in a trace.
        dao.admit_model(model_id, 0);
        dao.delete_model(model_id);
    });
}

}  // namespace agora::core::semantic_index
