// Port of data/local/ChatSearchDao.kt.
//
// Every member is a read-only @Query; the SQL text below is the annotation text unchanged, and
// the two collection filters expand exactly like Room's generated adapter (a placeholder per
// element, NULL for an empty list). Nothing here chooses search eligibility on its own: the
// predicates are the declared SQL.
#include "agora/core/chat_search_dao.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::chat_search {

namespace {

namespace json = agora::core::json;
namespace recovery = agora::core::run_recovery;

// ---- wire conversions (Room maps enum columns with Enum.valueOf) ---------------------------

recovery::MessageStatus message_status_from_wire(const std::string& wire) {
    if (wire == "TRANSCRIBING") return recovery::MessageStatus::transcribing;
    if (wire == "SENDING") return recovery::MessageStatus::sending;
    if (wire == "THINKING") return recovery::MessageStatus::thinking;
    if (wire == "TOOL_CALLING") return recovery::MessageStatus::tool_calling;
    if (wire == "SUCCESS") return recovery::MessageStatus::success;
    if (wire == "STOPPED") return recovery::MessageStatus::stopped;
    if (wire == "ERROR") return recovery::MessageStatus::error;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.MessageStatus." + wire);
}

recovery::Participant participant_from_wire(const std::string& wire) {
    if (wire == "USER") return recovery::Participant::user;
    if (wire == "MODEL") return recovery::Participant::model;
    if (wire == "ERROR") return recovery::Participant::error;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.Participant." + wire);
}

/** MessageConverters.toStringList: JSON array with the legacy "|||" format as fallback. */
std::vector<std::string> decode_string_list(const std::string& raw) {
    if (raw.empty()) return {};
    const json::ParseResult parsed = json::parse(raw);
    if (parsed.value.has_value() && parsed.value->kind == json::Kind::Array) {
        std::vector<std::string> out;
        for (const json::Value& element : parsed.value->elements) {
            if (element.kind != json::Kind::String) {
                throw KotlinError("SerializationException", "unexpected JSON element");
            }
            out.push_back(element.text);
        }
        return out;
    }
    // Backward compatibility: old format used "|||" delimiter.
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = raw.find("|||", start);
        if (at == std::string::npos) {
            parts.push_back(raw.substr(start));
            break;
        }
        parts.push_back(raw.substr(start, at - start));
        start = at + 3;
    }
    return parts;
}

// ---- row mapping --------------------------------------------------------------------------

SqlValue optional_text(const std::optional<std::string>& value) {
    return value.has_value() ? SqlValue::text_value(*value) : SqlValue::null();
}

std::optional<int> cell_int(const SqlValue& cell) {
    const std::optional<std::int64_t> value = cell.integer_or_null();
    if (!value.has_value()) return std::nullopt;
    return static_cast<int>(*value);
}

/** `SELECT m.* FROM messages m`: the stand schema's declaration/column order. */
MessageEntity message_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 26, "MessageEntity: row shape mismatch");
    MessageEntity message;
    message.id = row[0].as_text();
    message.conversation_id = row[1].as_text();
    message.parent_id = row[2].text_or_null();
    message.text = row[3].as_text();
    message.images = decode_string_list(row[4].as_text());
    message.thoughts = row[5].text_or_null();
    message.thought_title = row[6].text_or_null();
    message.token_count = static_cast<int>(row[7].as_integer());
    message.input_token_count = cell_int(row[8]);
    message.cached_input_token_count = cell_int(row[9]);
    message.cache_write_input_token_count = cell_int(row[10]);
    message.uncached_input_token_count = cell_int(row[11]);
    message.output_token_count = cell_int(row[12]);
    message.reasoning_token_count = cell_int(row[13]);
    message.generation_duration_ms = row[14].integer_or_null();
    message.status = message_status_from_wire(row[15].as_text());
    message.participant = participant_from_wire(row[16].as_text());
    message.timestamp = row[17].as_integer();
    message.thought_time_ms = row[18].integer_or_null();
    message.model_name = row[19].text_or_null();
    message.tool_call_json = row[20].text_or_null();
    message.attachment_meta = row[21].text_or_null();
    message.run_id = row[22].as_text();
    message.run_sequence = row[23].as_integer();
    message.consumed_at_pass = cell_int(row[24]);
    message.source_json = row[25].text_or_null();
    return message;
}

/** `SELECT * FROM conversations`: the stand schema's declaration/column order. */
ChatEntity conversation_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 15, "ChatEntity: row shape mismatch");
    ChatEntity conversation;
    conversation.id = row[0].as_text();
    conversation.title = row[1].as_text();
    conversation.last_updated = row[2].as_integer();
    conversation.data_changed_at = row[3].as_integer();
    conversation.selected_branches_json = row[4].text_or_null();
    conversation.system_prompt_id = row[5].text_or_null();
    conversation.model_id = row[6].text_or_null();
    conversation.task_id = row[7].text_or_null();
    conversation.origin = row[8].as_text();
    conversation.graduated = row[9].as_integer() != 0;
    conversation.draft_text = row[10].as_text();
    conversation.draft_attachments = row[11].text_or_null();
    conversation.selected_run_branches_json = row[12].text_or_null();
    conversation.has_unread_generation = row[13].as_integer() != 0;
    conversation.is_pinned = row[14].as_integer() != 0;
    return conversation;
}

// ---- collection parameters ----------------------------------------------------------------

/** The IN (...) placeholder list; an empty collection binds NULL (Room's expansion shape). */
std::string placeholders(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out.push_back(',');
        out.push_back('?');
    }
    if (out.empty()) out = "NULL";
    return out;
}

SqlArgs text_list(const std::vector<std::string>& values) {
    SqlArgs args;
    args.reserve(values.size());
    for (const std::string& value : values) args.push_back(SqlValue::text_value(value));
    return args;
}

int single_count(const std::vector<SqlRow>& rows) {
    kotlin_check(rows.size() == 1 && rows.front().size() == 1, "expected a single count row");
    return static_cast<int>(rows.front().front().as_integer());
}

}  // namespace

// ---- search eligibility -------------------------------------------------------------------

std::vector<MessageEntity> ChatSearchDao::search_messages(std::string_view query, int limit) {
    const std::string sql =
        "SELECT m.* FROM messages m INNER JOIN conversations c ON m.conversationId = c.id "
        "WHERE c.taskId IS NULL AND (m.text LIKE '%' || ? || '%' ESCAPE '\\' OR c.title LIKE "
        "'%' || ? || '%' ESCAPE '\\') AND m.participant IN ('USER', 'MODEL') AND m.text != '' "
        "AND substr(m.id, 1, 5) != 'tool_' AND substr(m.id, 1, 7) != 'result_' AND substr(m.id, "
        "1, 8) != 'compact_' ORDER BY m.timestamp DESC, m.id DESC LIMIT ?";
    const std::string text(query);
    const std::vector<SqlRow> rows = sql_.query(
        sql, {SqlValue::text_value(text), SqlValue::text_value(text),
              SqlValue::integer_value(limit)});
    std::vector<MessageEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(message_from_row(row));
    return out;
}

std::vector<MessageEntity> ChatSearchDao::get_messages_with_citation_segments_page(
    std::string_view after_id,
    int page_size) {
    const std::string sql =
        "SELECT m.* FROM messages m INNER JOIN conversations c ON m.conversationId = c.id "
        "WHERE c.taskId IS NULL AND m.toolCallJson LIKE '%\"type\":\"citation\"%' AND "
        "m.participant = 'MODEL' AND substr(m.id, 1, 5) != 'tool_' AND substr(m.id, 1, 7) != "
        "'result_' AND m.id > ? ORDER BY m.id ASC LIMIT ?";
    const std::vector<SqlRow> rows = sql_.query(
        sql, {SqlValue::text_value(std::string(after_id)), SqlValue::integer_value(page_size)});
    std::vector<MessageEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(message_from_row(row));
    return out;
}

std::vector<EmbeddingSearchRow> ChatSearchDao::get_embedding_search_page(
    std::string_view model_id,
    std::int64_t after_id,
    int minimum_text_length,
    int limit) {
    const std::string sql =
        "SELECT e.id, e.messageId, e.embedding, e.dimension FROM embeddings e CROSS JOIN "
        "messages m CROSS JOIN conversations c WHERE e.messageId = m.id AND m.conversationId = "
        "c.id AND e.modelId = ? AND e.id > ? AND c.taskId IS NULL AND m.participant IN ('USER', "
        "'MODEL') AND LENGTH(m.text) >= ? AND m.id NOT LIKE 'tool_%' AND m.id NOT LIKE "
        "'result_%' AND m.id NOT LIKE 'compact_%' ORDER BY e.id LIMIT ?";
    const std::vector<SqlRow> rows = sql_.query(
        sql, {SqlValue::text_value(std::string(model_id)), SqlValue::integer_value(after_id),
              SqlValue::integer_value(minimum_text_length), SqlValue::integer_value(limit)});
    std::vector<EmbeddingSearchRow> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        kotlin_check(row.size() == 4, "EmbeddingSearchRow: row shape mismatch");
        EmbeddingSearchRow entry;
        entry.id = row[0].as_integer();
        entry.message_id = row[1].as_text();
        entry.embedding = row[2].as_blob();
        entry.dimension = static_cast<int>(row[3].as_integer());
        out.push_back(std::move(entry));
    }
    return out;
}

int ChatSearchDao::get_embedding_count_by_model(std::string_view model_id) {
    const std::string sql =
        "SELECT COUNT(*) FROM embeddings e INNER JOIN messages m ON e.messageId = m.id INNER "
        "JOIN conversations c ON m.conversationId = c.id WHERE e.modelId = ? AND c.taskId IS "
        "NULL AND m.participant IN ('USER', 'MODEL') AND m.text != '' AND m.id NOT LIKE 'tool_%' "
        "AND m.id NOT LIKE 'result_%' AND m.id NOT LIKE 'compact_%'";
    return single_count(sql_.query(sql, {SqlValue::text_value(std::string(model_id))}));
}

std::vector<EmbeddingModelCount> ChatSearchDao::get_embedding_counts_by_models(
    const std::vector<std::string>& model_ids) {
    const std::string sql =
        "SELECT e.modelId AS modelId, COUNT(*) AS count FROM embeddings e INNER JOIN messages m "
        "ON e.messageId = m.id INNER JOIN conversations c ON m.conversationId = c.id WHERE "
        "e.modelId IN (" +
        placeholders(model_ids.size()) +
        ") AND c.taskId IS NULL AND m.participant IN ('USER', 'MODEL') AND m.text != '' AND "
        "m.id NOT LIKE 'tool_%' AND m.id NOT LIKE 'result_%' AND m.id NOT LIKE 'compact_%' "
        "GROUP BY e.modelId";
    const std::vector<SqlRow> rows = sql_.query(sql, text_list(model_ids));
    std::vector<EmbeddingModelCount> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        kotlin_check(row.size() == 2, "EmbeddingModelCount: row shape mismatch");
        EmbeddingModelCount entry;
        entry.model_id = row[0].as_text();
        entry.count = static_cast<int>(row[1].as_integer());
        out.push_back(std::move(entry));
    }
    return out;
}

int ChatSearchDao::get_indexable_message_count() {
    const std::string sql =
        "SELECT COUNT(*) FROM messages m INNER JOIN conversations c ON m.conversationId = c.id "
        "WHERE c.taskId IS NULL AND m.participant IN ('USER', 'MODEL') AND m.text != '' AND "
        "m.id NOT LIKE 'tool_%' AND m.id NOT LIKE 'result_%' AND m.id NOT LIKE 'compact_%'";
    return single_count(sql_.query(sql, {}));
}

std::vector<IndexableMessage> ChatSearchDao::get_searchable_messages_page(
    const std::optional<std::string>& after_id,
    int limit) {
    const std::string sql =
        "SELECT m.id, m.text FROM messages m INNER JOIN conversations c ON m.conversationId = "
        "c.id WHERE c.taskId IS NULL AND m.participant IN ('USER', 'MODEL') AND m.text != '' "
        "AND m.id NOT LIKE 'tool_%' AND m.id NOT LIKE 'result_%' AND m.id NOT LIKE 'compact_%' "
        "AND (? IS NULL OR m.id > ?) ORDER BY m.id LIMIT ?";
    const SqlValue after = optional_text(after_id);
    const std::vector<SqlRow> rows =
        sql_.query(sql, {after, after, SqlValue::integer_value(limit)});
    std::vector<IndexableMessage> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        kotlin_check(row.size() == 2, "IndexableMessage: row shape mismatch");
        IndexableMessage message;
        message.id = row[0].as_text();
        message.text = row[1].as_text();
        out.push_back(std::move(message));
    }
    return out;
}

std::vector<IndexableMessage> ChatSearchDao::get_unembedded_messages_page(
    std::string_view model_id,
    const std::optional<std::string>& after_id,
    int limit) {
    const std::string sql =
        "SELECT m.id, m.text FROM messages m INNER JOIN conversations c ON m.conversationId = "
        "c.id WHERE c.taskId IS NULL AND m.participant IN ('USER', 'MODEL') AND m.text != '' "
        "AND m.id NOT LIKE 'tool_%' AND m.id NOT LIKE 'result_%' AND m.id NOT LIKE "
        "'compact_%' AND NOT EXISTS (SELECT 1 FROM embeddings e WHERE e.messageId = m.id AND "
        "e.modelId = ?) AND (? IS NULL OR m.id > ?) ORDER BY m.id LIMIT ?";
    const SqlValue after = optional_text(after_id);
    const std::vector<SqlRow> rows = sql_.query(
        sql, {SqlValue::text_value(std::string(model_id)), after, after,
              SqlValue::integer_value(limit)});
    std::vector<IndexableMessage> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        kotlin_check(row.size() == 2, "IndexableMessage: row shape mismatch");
        IndexableMessage message;
        message.id = row[0].as_text();
        message.text = row[1].as_text();
        out.push_back(std::move(message));
    }
    return out;
}

std::vector<MessageEntity> ChatSearchDao::get_searchable_messages_by_ids(
    const std::vector<std::string>& ids) {
    const std::string sql =
        "SELECT m.* FROM messages m INNER JOIN conversations c ON m.conversationId = c.id "
        "WHERE m.id IN (" +
        placeholders(ids.size()) +
        ") AND c.taskId IS NULL AND m.participant IN ('USER', 'MODEL') AND m.text != '' AND "
        "m.id NOT LIKE 'tool_%' AND m.id NOT LIKE 'result_%' AND m.id NOT LIKE 'compact_%'";
    const std::vector<SqlRow> rows = sql_.query(sql, text_list(ids));
    std::vector<MessageEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(message_from_row(row));
    return out;
}

bool ChatSearchDao::is_message_searchable(std::string_view message_id) {
    const std::string sql =
        "SELECT EXISTS(SELECT 1 FROM messages m INNER JOIN conversations c ON "
        "m.conversationId = c.id WHERE m.id = ? AND c.taskId IS NULL AND m.participant IN "
        "('USER', 'MODEL') AND m.text != '' AND m.id NOT LIKE 'tool_%' AND m.id NOT LIKE "
        "'result_%' AND m.id NOT LIKE 'compact_%')";
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::text_value(std::string(message_id))});
    kotlin_check(rows.size() == 1 && rows.front().size() == 1, "EXISTS: row shape mismatch");
    return rows.front().front().as_integer() != 0;
}

// ---- conversation surface -----------------------------------------------------------------

std::optional<ChatEntity> ChatSearchDao::get_searchable_conversation(
    std::string_view conversation_id) {
    const std::string sql =
        "SELECT * FROM conversations WHERE id = ? AND taskId IS NULL";
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    return conversation_from_row(rows.front());
}

int ChatSearchDao::get_searchable_conversation_count() {
    return single_count(sql_.query("SELECT COUNT(*) FROM conversations WHERE taskId IS NULL", {}));
}

std::vector<ChatEntity> ChatSearchDao::get_searchable_conversations_page_ascending(int offset,
                                                                                   int limit) {
    const std::string sql =
        "SELECT * FROM conversations WHERE taskId IS NULL ORDER BY lastUpdated ASC, id ASC "
        "LIMIT ? OFFSET ?";
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::integer_value(limit), SqlValue::integer_value(offset)});
    std::vector<ChatEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(conversation_from_row(row));
    return out;
}

std::vector<ChatEntity> ChatSearchDao::get_searchable_conversations_page_descending(int offset,
                                                                                    int limit) {
    const std::string sql =
        "SELECT * FROM conversations WHERE taskId IS NULL ORDER BY lastUpdated DESC, id DESC "
        "LIMIT ? OFFSET ?";
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::integer_value(limit), SqlValue::integer_value(offset)});
    std::vector<ChatEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(conversation_from_row(row));
    return out;
}

}  // namespace agora::core::chat_search
