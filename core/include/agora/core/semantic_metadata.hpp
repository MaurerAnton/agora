#pragma once

// Port of data/local/SemanticMetadataDao.kt, SemanticSourceMetadata.kt and the public write half of
// SemanticMutationDao.kt: every conversation/message write owns its derived semantic input (the
// source rows, the per-conversation revisions and the ledger bumps), while the raw row primitives
// stay in ChatDao. The golden stand replaces only Room's annotation processor; the conditions here
// mirror the @Transaction default-method bodies verbatim, so an import emits exactly the
// semantic_index_ledger and embeddings rows the Kotlin importer does.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/chat_dao.hpp"
#include "agora/core/run_recovery.hpp"
#include "agora/core/semantic_index_ledger.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::semantic_metadata {

/** SemanticSourceEntity (data/local/SemanticSourceMetadata.kt). */
struct SemanticSourceEntity {
    std::string message_id;
    std::string conversation_id;
    bool eligible = false;
    std::vector<std::uint8_t> source_prefix;
    std::optional<std::string> source_fingerprint;
    /** SQLite LENGTH(TEXT) semantics: code points up to the first NUL. */
    int search_length = 0;
    bool history_eligible = false;
};

/** SemanticConversationEntity (data/local/SemanticSourceMetadata.kt). */
struct SemanticConversationRow {
    std::string conversation_id;
    std::int64_t source_revision = 0;
    int eligible_count = 0;
    bool cache_eligible = false;
};

/** semanticSource(messageId, conversationId, participant, text) from SemanticSourceMetadata.kt. */
SemanticSourceEntity semantic_source(std::string_view message_id, std::string_view conversation_id,
                                     run_recovery::Participant participant, std::string_view text);

/** Port of the SemanticMetadataDao surface the public writes call. */
class SemanticMetadataDao {
public:
    explicit SemanticMetadataDao(SqlExecutor& sql) : sql_(sql) {}

    /** ensureMetadataState(): the one-time legacy bootstrap cursor, inserted on first use. */
    void ensure_metadata_state();

    std::optional<SemanticConversationRow> get_semantic_conversation(
        std::string_view conversation_id);

    void ensure_semantic_conversation(std::string_view conversation_id);
    void advance_semantic_conversation(std::string_view conversation_id, int delta);
    void set_semantic_conversation_eligibility(std::string_view conversation_id, bool eligible);
    void mark_semantic_models_pending(std::int64_t at);

    /** maintainSemanticSource(source, legacy) — the @Transaction default method. */
    void maintain_semantic_source(const SemanticSourceEntity& source, bool legacy,
                                  std::int64_t at);
    /** removeSemanticSource(messageId). */
    void remove_semantic_source(std::string_view message_id, std::int64_t at);

    /** SemanticEmbeddingDao.deleteEmbeddingsByConversation. */
    void delete_embeddings_by_conversation(std::string_view conversation_id);

    /** advanceLegacyMetadata(lastMessageId, exhausted) over the bootstrap cursor. */
    void advance_legacy_metadata(std::optional<std::string> last_message_id, bool exhausted);

private:
    SqlExecutor& sql_;

    std::optional<SemanticSourceEntity> get_semantic_source(std::string_view message_id);
    void put_semantic_source(const SemanticSourceEntity& source);
    struct EmbeddingIdentity {
        std::string message_id;
        std::string model_id;
    };
    std::vector<EmbeddingIdentity> get_source_embedding_identities(std::string_view message_id);
    void adjust_cached_count(const std::string& model_id, const std::string& conversation_id,
                             int delta);
    void adjust_source_embedding_counts(const SemanticSourceEntity& source, int delta);
    void delete_source_embedding_rows(std::string_view message_id);
};

/**
 * withSemanticGraphMutation's leading `snapshot.second.forEach { insertLedger(...) }`: every
 * configured model is admitted with the entity defaults (NEEDS_RECONCILE, revisions 0) and
 * OnConflictStrategy.IGNORE keeps rows that already exist.
 */
void insert_snapshot_ledger(semantic_index::SemanticIndexDao& dao,
                            const semantic_index::SemanticModelSnapshot& snapshot,
                            std::int64_t updated_at);

// ---- SemanticMutationDao's public write surface --------------------------------------------
// The Kotlin names resolve to the @Transaction default methods that wrap the ChatDao row writes.

/** SemanticMutationDao.upsertConversation(conversation). */
void upsert_conversation(chat_dao::ChatDao& dao, SemanticMetadataDao& meta,
                         const ChatEntity& conversation, std::int64_t at);
/** SemanticMutationDao.upsertMessage(message). */
void upsert_message(chat_dao::ChatDao& dao, SemanticMetadataDao& meta, const MessageEntity& message,
                    std::int64_t at);
/** SemanticMutationDao.deleteAllConversations(): rows plus the derived bookkeeping. */
void delete_all_conversations(chat_dao::ChatDao& dao, semantic_index::SemanticIndexDao& index,
                              SemanticMetadataDao& meta, std::int64_t at);

}  // namespace agora::core::semantic_metadata
