#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::semantic_index {

// ---- Kotlin validation failures ----------------------------------------------------------

/**
 * A Kotlin `require`/`check`/`checkNotNull` failure surfaced with the JVM exception class,
 * because the golden driver records `{"error": class, "message": message}` for every step.
 */
class KotlinError : public std::runtime_error {
public:
    KotlinError(std::string class_name, std::string message)
        : std::runtime_error(std::move(message)), class_name_(std::move(class_name)) {}

    const std::string& class_name() const { return class_name_; }

private:
    std::string class_name_;
};

/** Kotlin require(value) / require(value) { message }. */
inline void kotlin_require(bool satisfied, std::string message = "Failed requirement.") {
    if (!satisfied) throw KotlinError("IllegalArgumentException", std::move(message));
}

/** Kotlin check(value) / checkNotNull(value) { message }. */
inline void kotlin_check(bool satisfied, std::string message = "Check failed.") {
    if (!satisfied) throw KotlinError("IllegalStateException", std::move(message));
}

// ---- SQL execution surface ---------------------------------------------------------------

/** A bound parameter or result cell with the SQLite runtime type preserved. */
struct SqlValue {
    enum class Kind {
        Null,
        Text,
        Integer,
        Real,
        Blob,
    };

    Kind kind = Kind::Null;
    std::string text;
    std::int64_t integer = 0;
    double real = 0.0;
    std::vector<std::uint8_t> blob;

    static SqlValue null();
    static SqlValue text_value(std::string value);
    static SqlValue integer_value(std::int64_t value);
    static SqlValue bool_value(bool value);

    bool is_null() const { return kind == Kind::Null; }
    const std::string& as_text() const;
    std::int64_t as_integer() const;
    std::vector<std::uint8_t> as_blob() const;
    std::optional<std::string> text_or_null() const;
    std::optional<std::int64_t> integer_or_null() const;
};

using SqlArgs = std::vector<SqlValue>;
using SqlRow = std::vector<SqlValue>;

/** A constraint rejection (SQLITE_CONSTRAINT): Room's @Upsert falls back to update-by-PK. */
class SqlConstraintError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Any other SQLite failure — only reachable through a bug in the ported SQL. */
class SqlError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * The SQLite execution boundary. Room is replaced by this surface the same way the clone stand
 * replaced the file system: the port owns the SQL text (transcribed from the @Query annotations)
 * and every conditional sequence, and the caller supplies the database.
 */
class SqlExecutor {
public:
    virtual ~SqlExecutor() = default;

    /** Runs a statement without result rows; returns the number of changed rows. */
    virtual std::int64_t execute(const std::string& sql, const SqlArgs& args) = 0;

    /** Runs a query; rows carry their cells in column order. */
    virtual std::vector<SqlRow> query(const std::string& sql, const SqlArgs& args) = 0;

    /** sqlite3_last_insert_rowid() for this connection. */
    virtual std::int64_t last_insert_rowid() = 0;

    /** SQLite transaction control; begin/commit/rollback arrive in matched pairs. */
    virtual void begin() = 0;
    virtual void commit() = 0;
    virtual void rollback() = 0;
};

// ---- entity rows ------------------------------------------------------------------------

/** SemanticIndexLedgerEntity (data/local/SemanticIndexLedger.kt), validated on construction. */
struct LedgerEntry {
    std::string model_id;
    std::string state = "NEEDS_RECONCILE";
    std::int64_t source_revision = 0;
    std::int64_t completed_revision = 0;
    std::int64_t reconcile_revision = 0;
    std::int64_t updated_at = 0;

    /** The entity's init block: every `require` of SemanticIndexLedgerEntity. */
    void validate() const;
};

/** SemanticIndexWorkEntity, validated on construction. */
struct WorkEntry {
    std::string model_id;
    std::string message_id;
    std::optional<std::string> source_fingerprint;
    std::int64_t source_revision = 0;
    std::int64_t updated_at = 0;

    /** The entity's init block: every `require` of SemanticIndexWorkEntity. */
    void validate() const;
};

/** One row of getReconcileMessagesPage. */
struct ReconcileIndexableMessage {
    std::string id;
    std::string text;
    std::optional<std::int64_t> embedding_id;
    std::optional<std::string> embedding_fingerprint;
    std::optional<std::int64_t> dimension;
    std::optional<std::int64_t> embedding_bytes;
};

/** EmbeddingEntity (data/local/ChatEntities.kt) — no init constraints in Kotlin. */
struct EmbeddingEntry {
    std::int64_t id = 0;
    std::string message_id;
    std::string model_id;
    std::vector<std::uint8_t> embedding;
    std::string chunk_text;
    std::int64_t dimension = 0;
    std::optional<std::string> source_fingerprint;
};

/** SemanticModelSnapshot = Pair<String?, Set<String>>. */
struct SemanticModelSnapshot {
    std::optional<std::string> active_model_id;
    std::vector<std::string> configured_model_ids;
};

// ---- the DAO ----------------------------------------------------------------------------

/**
 * Port of data/local/SemanticIndexLedger.kt: the @Query SQL text, the @Insert/@Upsert conflict
 * algorithms and the @Transaction default-method bodies. It runs against a SqlExecutor; Room's
 * annotation processor is the only replaced platform.
 */
class SemanticIndexDao {
public:
    explicit SemanticIndexDao(SqlExecutor& sql) : sql_(sql) {}

    // -- @Transaction nesting (RoomDatabase.beginTransaction/setTransactionSuccessful/endTransaction)

    void begin_transaction();
    void set_transaction_successful();
    void end_transaction();

    // -- plain DAO methods -----------------------------------------------------------------

    /** @Insert(onConflict = IGNORE): -1 when the row was ignored. */
    std::int64_t insert_ledger(const LedgerEntry& ledger);

    std::optional<LedgerEntry> get_ledger(std::string_view model_id);

    std::vector<LedgerEntry> get_ledgers(const std::vector<std::string>& model_ids);

    std::int64_t advance_for_exact_work(std::string_view model_id, std::int64_t updated_at);

    std::int64_t advance_for_reconcile(std::string_view model_id, std::int64_t updated_at);

    /** @Upsert: insert, and on a constraint conflict update the row with the same primary key. */
    void upsert_work(const WorkEntry& work);

    std::optional<WorkEntry> get_work(std::string_view model_id, std::string_view message_id);

    std::vector<WorkEntry> get_work_page(std::string_view model_id,
                                         std::int64_t max_source_revision,
                                         std::int64_t after_source_revision,
                                         std::string_view after_message_id,
                                         std::int32_t limit);

    std::int64_t get_work_count_through_revision(std::string_view model_id,
                                                std::int64_t max_source_revision);

    std::vector<ReconcileIndexableMessage> get_reconcile_messages_page(
        std::string_view model_id,
        std::int64_t max_work_revision,
        std::optional<std::string> after_id,
        std::int32_t limit);

    std::optional<std::string> get_searchable_message_text(std::string_view message_id);

    void upsert_embedding(const EmbeddingEntry& embedding);

    std::int64_t delete_embeddings_for_messages(const std::vector<std::string>& message_ids);

    std::int64_t delete_embeddings_for_model(std::string_view model_id);

    std::int64_t delete_all_embeddings();

    std::int64_t delete_work_for_model(std::string_view model_id);

    std::int64_t delete_matching_work(std::string_view model_id,
                                      std::string_view message_id,
                                      std::optional<std::string> source_fingerprint,
                                      std::int64_t source_revision);

    std::int64_t mark_current_after_exact_work(std::string_view model_id,
                                              std::int64_t source_revision,
                                              std::int64_t updated_at);

    std::int64_t mark_after_reconcile(std::string_view model_id,
                                      std::int64_t expected_reconcile_revision,
                                      std::int64_t updated_at);

    std::int64_t delete_model(std::string_view model_id);

    // -- @Transaction default methods (bodies verbatim) -------------------------------------

    LedgerEntry admit_model(std::string_view model_id, std::int64_t updated_at);

    LedgerEntry request_reconcile(std::string_view model_id, std::int64_t updated_at);

    LedgerEntry enqueue_exact_work(std::string_view model_id,
                                   std::string_view message_id,
                                   std::optional<std::string> source_fingerprint,
                                   std::int64_t updated_at);

    bool complete_exact_work(const WorkEntry& work, std::int64_t updated_at);

    bool complete_reconcile(std::string_view model_id,
                            std::int64_t expected_reconcile_revision,
                            std::int64_t updated_at);

private:
    SqlExecutor& sql_;
    int depth_ = 0;
    bool aborted_ = false;
    std::vector<bool> level_successful_;
};

// ---- ChatDao surface the orchestration reads ---------------------------------------------

/**
 * The conversation rows `withSemanticEligibilityMutation` observes. Kotlin reads
 * `chatDao().getConversation(id)?.let { it.taskId == null }` and calls
 * `chatDao().deleteEmbeddingsByConversation(id)`; both queries belong to ChatDao (another
 * module), so this is the documented boundary of the ledger.
 */
class ConversationSurface {
public:
    virtual ~ConversationSurface() = default;

    /** getConversation(id)?.let { it.taskId == null }; nullopt when the row does not exist. */
    virtual std::optional<bool> conversation_is_task_free(std::string_view conversation_id) = 0;

    /** chatDao().deleteEmbeddingsByConversation(id) */
    virtual void delete_embeddings_by_conversation(std::string_view conversation_id) = 0;
};

// ---- pure functions ----------------------------------------------------------------------

/** semanticSourceFingerprint(text): SHA-256 of the first 8000 UTF-16 units as UTF-8 hex. */
std::string semantic_source_fingerprint(std::string_view text);

/** semanticModelSnapshot(activeModelId, configuredModelIds). */
SemanticModelSnapshot semantic_model_snapshot(
    const std::optional<std::string>& active_model_id,
    const std::vector<std::string>& configured_model_ids);

// ---- ChatDatabase extension functions ----------------------------------------------------

/** requestSemanticReconcile(snapshot, updatedAt) — withTransaction wrapped. */
void request_semantic_reconcile(SemanticIndexDao& dao,
                                const SemanticModelSnapshot& snapshot,
                                std::int64_t updated_at);

/** withSemanticSourceMutation(snapshot, messageIds, updatedAt) { block }. */
void with_semantic_source_mutation(SemanticIndexDao& dao,
                                   const SemanticModelSnapshot& snapshot,
                                   const std::vector<std::string>& message_ids,
                                   std::int64_t updated_at,
                                   const std::function<void()>& block);

/** withSemanticGraphMutation(snapshot, clearMessageIds, clearAllEmbeddings, updatedAt) { block }. */
void with_semantic_graph_mutation(SemanticIndexDao& dao,
                                  const SemanticModelSnapshot& snapshot,
                                  const std::vector<std::string>& clear_message_ids,
                                  bool clear_all_embeddings,
                                  std::int64_t updated_at,
                                  const std::function<void()>& block);

/** withSemanticEligibilityMutation(snapshot, conversationId, updatedAt) { block }. */
void with_semantic_eligibility_mutation(SemanticIndexDao& dao,
                                        ConversationSurface& conversations,
                                        const SemanticModelSnapshot& snapshot,
                                        std::string_view conversation_id,
                                        std::int64_t updated_at,
                                        const std::function<void()>& block);

/** commitSemanticEmbedding(...): withTransaction wrapped, false on a superseded generation. */
bool commit_semantic_embedding(SemanticIndexDao& dao,
                               const EmbeddingEntry& embedding,
                               std::string_view expected_fingerprint,
                               std::int64_t updated_at,
                               std::optional<std::int64_t> expected_work_revision,
                               std::optional<std::int64_t> expected_reconcile_revision,
                               bool complete_pending_work);

/** invalidateSemanticModel(modelId, updatedAt): withTransaction wrapped. */
void invalidate_semantic_model(SemanticIndexDao& dao,
                               std::string_view model_id,
                               std::int64_t updated_at);

/** deleteSemanticModel(modelId): withTransaction wrapped. */
void delete_semantic_model(SemanticIndexDao& dao, std::string_view model_id);

}  // namespace agora::core::semantic_index
