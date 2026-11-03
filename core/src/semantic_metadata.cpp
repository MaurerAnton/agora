#include "agora/core/semantic_metadata.hpp"

#include <cstddef>

#include "agora/core/json.hpp"

namespace agora::core::semantic_metadata {
namespace {

namespace chat = agora::core::chat_dao;
namespace ledger = agora::core::semantic_index;

/** Constants.MAX_EMBEDDING_TEXT_LENGTH — the provider input and fingerprint boundary. */
constexpr int kMaxEmbeddingTextLength = 8000;

SqlValue blob_value(const std::vector<std::uint8_t>& bytes) {
    SqlValue out;
    out.kind = SqlValue::Kind::Blob;
    out.blob = bytes;
    return out;
}

/** SQLite LIKE folds only ASCII; the synthetic prefixes compare lowercased (prefix stays lower). */
bool has_lower_prefix(std::string_view text, std::string_view prefix) {
    if (text.size() <= prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        char character = text[index];
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        if (character != prefix[index]) return false;
    }
    return true;
}

bool has_prefix(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/** text.codePointCount(0, end) over the bytes before the first NUL. */
int code_point_count(std::string_view text) {
    int count = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) ++count;
    }
    return count;
}

void insert_ledger_row(ledger::SemanticIndexDao& dao, std::string_view model_id,
                       std::int64_t updated_at) {
    ledger::LedgerEntry entry;
    entry.model_id = std::string(model_id);
    entry.updated_at = updated_at;
    entry.validate();
    dao.insert_ledger(entry);
}

}  // namespace

SemanticSourceEntity semantic_source(std::string_view message_id, std::string_view conversation_id,
                                     run_recovery::Participant participant, std::string_view text) {
    // `listOf("tool", "result", "compact").any { ... }` — strict length plus ASCII-caseless match.
    const bool synthetic = has_lower_prefix(message_id, "tool") ||
                           has_lower_prefix(message_id, "result") ||
                           has_lower_prefix(message_id, "compact");
    const bool ordinary =
        !text.empty() &&
        (participant == run_recovery::Participant::user ||
         participant == run_recovery::Participant::model);
    const bool eligible = ordinary && !synthetic;

    SemanticSourceEntity source;
    source.message_id = std::string(message_id);
    source.conversation_id = std::string(conversation_id);
    source.eligible = eligible;
    // Keep the next UTF-16 unit so UTF-8 persistence cannot split a surrogate pair.
    if (eligible) {
        const std::string prefix = json::take_utf16_units(text, kMaxEmbeddingTextLength + 1);
        source.source_prefix.assign(prefix.begin(), prefix.end());
        source.source_fingerprint = ledger::semantic_source_fingerprint(prefix);
    }
    const std::size_t end = text.find('\0');
    source.search_length =
        ordinary ? code_point_count(end == std::string_view::npos ? text : text.substr(0, end)) : 0;
    source.history_eligible =
        ordinary && !has_prefix(message_id, "tool_") && !has_prefix(message_id, "result_") &&
        !has_prefix(message_id, "compact_");
    return source;
}

void SemanticMetadataDao::ensure_metadata_state() {
    const std::vector<SqlRow> rows =
        sql_.query("SELECT id FROM semantic_metadata_state WHERE id = 0", {});
    if (!rows.empty()) return;
    sql_.execute(
        "INSERT OR IGNORE INTO semantic_metadata_state(id, complete, afterMessageId) "
        "VALUES(0, NOT EXISTS(SELECT 1 FROM messages LIMIT 1), NULL)",
        {});
}

std::optional<SemanticConversationRow> SemanticMetadataDao::get_semantic_conversation(
    std::string_view conversation_id) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT sourceRevision, eligibleCount, cacheEligible FROM semantic_conversations "
        "WHERE conversationId = ?",
        {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    SemanticConversationRow row;
    row.conversation_id = std::string(conversation_id);
    row.source_revision = rows[0][0].as_integer();
    row.eligible_count = static_cast<int>(rows[0][1].as_integer());
    row.cache_eligible = rows[0][2].as_integer() != 0;
    return row;
}

void SemanticMetadataDao::ensure_semantic_conversation(std::string_view conversation_id) {
    sql_.execute(
        "INSERT OR IGNORE INTO semantic_conversations(conversationId, sourceRevision, "
        "eligibleCount, cacheEligible) SELECT id, 0, 0, taskId IS NULL FROM conversations "
        "WHERE id = ?",
        {SqlValue::text_value(std::string(conversation_id))});
}

void SemanticMetadataDao::advance_semantic_conversation(std::string_view conversation_id,
                                                        int delta) {
    sql_.execute(
        "UPDATE semantic_conversations SET sourceRevision = sourceRevision + 1, "
        "eligibleCount = eligibleCount + ? WHERE conversationId = ?",
        {SqlValue::integer_value(delta), SqlValue::text_value(std::string(conversation_id))});
}

void SemanticMetadataDao::set_semantic_conversation_eligibility(std::string_view conversation_id,
                                                                bool eligible) {
    sql_.execute(
        "UPDATE semantic_conversations SET cacheEligible = ? WHERE conversationId = ?",
        {SqlValue::bool_value(eligible),
         SqlValue::text_value(std::string(conversation_id))});
}

void SemanticMetadataDao::mark_semantic_models_pending(std::int64_t at) {
    sql_.execute(
        "UPDATE semantic_index_ledger SET state = CASE WHEN state = 'NEEDS_RECONCILE' THEN state "
        "ELSE 'PENDING' END, sourceRevision = sourceRevision + 1, updatedAt = :at "
        "WHERE state != 'DELETED'",
        {SqlValue::integer_value(at)});
}

std::optional<SemanticSourceEntity> SemanticMetadataDao::get_semantic_source(
    std::string_view message_id) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT messageId, conversationId, eligible, sourcePrefix, sourceFingerprint, "
        "searchLength, historyEligible FROM semantic_sources WHERE messageId = ?",
        {SqlValue::text_value(std::string(message_id))});
    if (rows.empty()) return std::nullopt;
    SemanticSourceEntity source;
    source.message_id = rows[0][0].as_text();
    source.conversation_id = rows[0][1].as_text();
    source.eligible = rows[0][2].as_integer() != 0;
    source.source_prefix = rows[0][3].as_blob();
    source.source_fingerprint = rows[0][4].text_or_null();
    source.search_length = static_cast<int>(rows[0][5].as_integer());
    source.history_eligible = rows[0][6].as_integer() != 0;
    return source;
}

void SemanticMetadataDao::put_semantic_source(const SemanticSourceEntity& source) {
    sql_.execute(
        "INSERT INTO semantic_sources (messageId, conversationId, eligible, sourcePrefix, "
        "sourceFingerprint, searchLength, historyEligible) VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(messageId) DO UPDATE SET conversationId = excluded.conversationId, "
        "eligible = excluded.eligible, sourcePrefix = excluded.sourcePrefix, "
        "sourceFingerprint = excluded.sourceFingerprint, "
        "searchLength = excluded.searchLength, historyEligible = excluded.historyEligible",
        {SqlValue::text_value(source.message_id), SqlValue::text_value(source.conversation_id),
         SqlValue::bool_value(source.eligible), blob_value(source.source_prefix),
         source.source_fingerprint.has_value()
             ? SqlValue::text_value(*source.source_fingerprint)
             : SqlValue::null(),
         SqlValue::integer_value(source.search_length),
         SqlValue::bool_value(source.history_eligible)});
}

std::vector<SemanticMetadataDao::EmbeddingIdentity>
SemanticMetadataDao::get_source_embedding_identities(std::string_view message_id) {
    // CURRENT_EMBEDDING from SemanticEmbeddingGeneration.kt, verbatim.
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT e.messageId, e.modelId FROM embeddings e WHERE e.messageId = ? AND "
        "e.generation = COALESCE((SELECT generation FROM semantic_index_ledger "
        "WHERE modelId = e.modelId), 0) "
        "AND NOT EXISTS (SELECT 1 FROM semantic_index_ledger "
        "WHERE modelId = e.modelId AND state = 'DELETED')",
        {SqlValue::text_value(std::string(message_id))});
    std::vector<EmbeddingIdentity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        out.push_back(EmbeddingIdentity{row[0].as_text(), row[1].as_text()});
    }
    return out;
}

void SemanticMetadataDao::adjust_cached_count(const std::string& model_id,
                                              const std::string& conversation_id, int delta) {
    sql_.execute(
        "INSERT OR IGNORE INTO semantic_model_conversations (modelId, conversationId, "
        "cachedCount, indexedRevision) VALUES (?, ?, 0, -1)",
        {SqlValue::text_value(model_id), SqlValue::text_value(conversation_id)});
    sql_.execute(
        "UPDATE semantic_model_conversations SET cachedCount = cachedCount + ? "
        "WHERE modelId = ? AND conversationId = ?",
        {SqlValue::integer_value(delta), SqlValue::text_value(model_id),
         SqlValue::text_value(conversation_id)});
}

void SemanticMetadataDao::adjust_source_embedding_counts(const SemanticSourceEntity& source,
                                                         int delta) {
    if (!source.eligible) return;
    for (const EmbeddingIdentity& identity :
         get_source_embedding_identities(source.message_id)) {
        adjust_cached_count(identity.model_id, source.conversation_id, delta);
    }
}

void SemanticMetadataDao::delete_source_embedding_rows(std::string_view message_id) {
    sql_.execute("DELETE FROM embeddings WHERE messageId = ?",
                 {SqlValue::text_value(std::string(message_id))});
}

void SemanticMetadataDao::maintain_semantic_source(const SemanticSourceEntity& source, bool legacy,
                                                   std::int64_t at) {
    const std::optional<SemanticSourceEntity> previous = get_semantic_source(source.message_id);
    ensure_semantic_conversation(source.conversation_id);
    // Kotlin's nullable comparisons: a missing previous row differs from every value.
    bool changed;
    bool semantic_changed;
    if (previous.has_value()) {
        changed = previous->eligible != source.eligible ||
                  previous->source_fingerprint != source.source_fingerprint ||
                  previous->conversation_id != source.conversation_id;
        semantic_changed = changed && (previous->eligible || source.eligible);
    } else {
        changed = true;
        semantic_changed = source.eligible;
    }
    if (!changed && previous.has_value() &&
        previous->search_length == source.search_length &&
        previous->history_eligible == source.history_eligible) {
        return;
    }
    if (semantic_changed) {
        if (previous.has_value()) adjust_source_embedding_counts(*previous, -1);
        if (previous.has_value() && previous->conversation_id != source.conversation_id) {
            advance_semantic_conversation(previous->conversation_id,
                                          previous->eligible ? -1 : 0);
        }
        const bool old_count = previous.has_value() &&
                               previous->conversation_id == source.conversation_id &&
                               previous->eligible;
        advance_semantic_conversation(source.conversation_id,
                                      (source.eligible ? 1 : 0) - (old_count ? 1 : 0));
        mark_semantic_models_pending(at);
    }
    // Bootstrap retains legacy vectors. A new write cannot prove an old vector matches.
    if (changed && !legacy) delete_source_embedding_rows(source.message_id);
    put_semantic_source(source);
    if (semantic_changed) adjust_source_embedding_counts(source, 1);
}

void SemanticMetadataDao::remove_semantic_source(std::string_view message_id,
                                                 std::int64_t at) {
    const std::optional<SemanticSourceEntity> previous = get_semantic_source(message_id);
    if (previous.has_value()) {
        adjust_source_embedding_counts(*previous, -1);
        if (previous->eligible) {
            advance_semantic_conversation(previous->conversation_id, -1);
            mark_semantic_models_pending(at);
        }
        sql_.execute("DELETE FROM semantic_sources WHERE messageId = ?",
                     {SqlValue::text_value(std::string(message_id))});
    }
    delete_source_embedding_rows(message_id);
}

void SemanticMetadataDao::delete_embeddings_by_conversation(std::string_view conversation_id) {
    sql_.execute(
        "DELETE FROM embeddings WHERE messageId IN "
        "(SELECT id FROM messages WHERE conversationId = ?)",
        {SqlValue::text_value(std::string(conversation_id))});
    sql_.execute(
        "UPDATE semantic_model_conversations SET cachedCount = 0, indexedRevision = -1 "
        "WHERE conversationId = ?",
        {SqlValue::text_value(std::string(conversation_id))});
}

void SemanticMetadataDao::advance_legacy_metadata(std::optional<std::string> last_message_id,
                                                  bool exhausted) {
    sql_.execute(
        "UPDATE semantic_metadata_state SET afterMessageId = ?, complete = ? WHERE id = 0",
        {last_message_id.has_value() ? SqlValue::text_value(*last_message_id) : SqlValue::null(),
         SqlValue::bool_value(exhausted)});
}

void insert_snapshot_ledger(ledger::SemanticIndexDao& dao,
                            const ledger::SemanticModelSnapshot& snapshot,
                            std::int64_t updated_at) {
    for (const std::string& model_id : snapshot.configured_model_ids) {
        insert_ledger_row(dao, model_id, updated_at);
    }
}

void upsert_conversation(chat::ChatDao& dao, SemanticMetadataDao& meta,
                         const ChatEntity& conversation, std::int64_t at) {
    meta.ensure_metadata_state();
    const std::optional<SemanticConversationRow> before =
        meta.get_semantic_conversation(conversation.id);
    dao.upsert_conversation(conversation);
    meta.ensure_semantic_conversation(conversation.id);
    const bool eligible = !conversation.task_id.has_value();
    if (before.has_value() && before->cache_eligible != eligible) {
        meta.set_semantic_conversation_eligibility(conversation.id, eligible);
        meta.advance_semantic_conversation(conversation.id, 0);
        if (!eligible) meta.delete_embeddings_by_conversation(conversation.id);
        meta.mark_semantic_models_pending(at);
    }
}

void upsert_message(chat::ChatDao& dao, SemanticMetadataDao& meta, const MessageEntity& message,
                    std::int64_t at) {
    meta.ensure_metadata_state();
    dao.upsert_message(message);
    meta.maintain_semantic_source(
        semantic_source(message.id, message.conversation_id, message.participant, message.text),
        /*legacy=*/false, at);
}

void delete_all_conversations(chat::ChatDao& dao, ledger::SemanticIndexDao& index,
                              SemanticMetadataDao& meta, std::int64_t at) {
    index.delete_all_embeddings();
    dao.delete_all_conversations();
    meta.advance_legacy_metadata(std::nullopt, true);
    meta.mark_semantic_models_pending(at);
}

}  // namespace agora::core::semantic_metadata
