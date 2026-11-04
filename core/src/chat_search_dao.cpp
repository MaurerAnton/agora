// Port of data/local/ChatSearchDao.kt.
//
// Every member is a read-only @Query; the SQL text below is the annotation text unchanged, and
// the two collection filters expand exactly like Room's generated adapter (a placeholder per
// element, NULL for an empty list). Nothing here chooses search eligibility on its own: the
// predicates are the declared SQL.
#include "agora/core/chat_search_dao.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/citations.hpp"
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
    // schema 46 grew (todoJson/parentConversationId/createdAt/subagent*); the projection still
    // reads the first fifteen columns in declaration order.
    kotlin_check(row.size() == 22, "ChatEntity: row shape mismatch");
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

namespace {

// ChatSearchDao.HISTORY_MESSAGE_ELIGIBILITY — shared by ranking, hydration and the searchability
// predicates (the aliases match the surrounding FROM clause).
constexpr std::string_view kHistoryEligibility =
    "c.taskId IS NULL AND c.parentConversationId IS NULL "
    "AND m.participant IN ('USER', 'MODEL') AND m.text != '' "
    "AND substr(m.id, 1, 5) != 'tool_' AND substr(m.id, 1, 7) != 'result_' "
    "AND substr(m.id, 1, 8) != 'compact_'";

/** ensureMetadataState + read of the legacy bootstrap cursor. */
bool metadata_state_complete(SqlExecutor& sql) {
    std::vector<SqlRow> rows = sql.query("SELECT complete FROM semantic_metadata_state WHERE id = 0", {});
    if (rows.empty()) {
        sql.execute(
            "INSERT OR IGNORE INTO semantic_metadata_state(id, complete, afterMessageId) "
            "VALUES(0, NOT EXISTS(SELECT 1 FROM messages LIMIT 1), NULL)",
            {});
        rows = sql.query("SELECT complete FROM semantic_metadata_state WHERE id = 0", {});
    }
    kotlin_check(rows.size() == 1 && rows.front().size() == 1,
                 "semantic_metadata_state: row shape mismatch");
    return rows.front().front().as_integer() != 0;
}

/** getEmbeddingCacheCounts: eligible total plus the requested models' semantic counters. */
struct CacheCounts {
    int total = 0;
    std::vector<std::pair<std::string, int>> per_model;  // requested order, duplicates removed
};

CacheCounts embedding_cache_counts(SqlExecutor& sql,
                                   const std::vector<std::string>& requested_ids) {
    std::vector<std::string> requested;
    for (const std::string& id : requested_ids) {
        if (std::find(requested.begin(), requested.end(), id) == requested.end()) {
            requested.push_back(id);
        }
    }
    if (!metadata_state_complete(sql)) {
        CacheCounts counts;
        for (const std::string& id : requested) counts.per_model.emplace_back(id, 0);
        return counts;
    }
    CacheCounts counts;
    for (const SqlRow& row : sql.query(
             "SELECT COALESCE(SUM(eligibleCount), 0) FROM semantic_conversations "
             "WHERE cacheEligible = 1",
             {})) {
        counts.total = static_cast<int>(row[0].as_integer());
    }
    std::map<std::string, int> cached;
    if (!requested.empty()) {
        std::string in;
        for (std::size_t index = 0; index < requested.size(); ++index) {
            if (index != 0) in += ", ";
            in += "?";
        }
        SqlArgs args;
        for (const std::string& id : requested) args.push_back(SqlValue::text_value(id));
        for (const SqlRow& row : sql.query(
                 "SELECT m.modelId, SUM(m.cachedCount) FROM semantic_model_conversations m "
                 "JOIN semantic_conversations c ON c.conversationId = m.conversationId "
                 "WHERE c.cacheEligible = 1 AND m.modelId IN (" +
                     in + ") GROUP BY m.modelId",
                 args)) {
            cached[row[0].as_text()] = static_cast<int>(row[1].as_integer());
        }
    }
    for (const std::string& id : requested) {
        const auto it = cached.find(id);
        counts.per_model.emplace_back(id, it == cached.end() ? 0 : it->second);
    }
    return counts;
}

const json::Value* json_field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == name) return &member.value;
    }
    return nullptr;
}

/** MessageSegment.toCitationRecord + citationRecords("") + matchesCitationTitle. */
bool citation_title_matches(const std::optional<std::string>& raw, std::string_view query) {
    if (!raw.has_value()) return false;
    const json::ParseResult parsed = json::parse(*raw);
    if (!parsed.value || parsed.value->kind != json::Kind::Array) return false;
    std::vector<citations::CitationRecord> records;
    for (const json::Value& segment : parsed.value->elements) {
        if (segment.kind != json::Kind::Object) continue;
        const json::Value* type = json_field(segment, "type");
        if (type == nullptr || type->kind != json::Kind::String || type->text != "citation") {
            continue;
        }
        const json::Value* content = json_field(segment, "content");
        if (content == nullptr || content->kind != json::Kind::String) continue;
        if (std::optional<citations::CitationRecord> record =
                citations::policy::decode(content->text, std::string{})) {
            records.push_back(std::move(*record));
        }
    }
    records = citations::policy::deduplicate(records, std::string{});
    return citations::matches_citation_title(records, std::string(query));
}

/** ChatSearchDao.escapeLikePattern. */
std::string escape_like_pattern(std::string_view query) {
    std::string out;
    out.reserve(query.size() + 8);
    for (const char character : query) {
        if (character == '\\' || character == '%' || character == '_') out.push_back('\\');
        out.push_back(character);
    }
    return out;
}

}  // namespace

// ---- search eligibility -------------------------------------------------------------------

std::vector<MessageEntity> ChatSearchDao::search_messages(std::string_view query, int limit) {
    // ConversationMessageSearch.searchConversationMessages: blank queries and non-positive limits
    // return nothing, then the bounded keyword cursor pages the LIKE/literal matcher and finally
    // hydrates only the winners through the history visibility boundary.
    if (json::is_blank(query) || limit <= 0) return {};
    const std::string text(query);
    const std::string escaped = escape_like_pattern(text);
    // SQLite LIKE folds only ASCII letters; other literals use the exact substring scan.
    const bool has_ascii_alpha =
        std::any_of(text.begin(), text.end(), [](char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z');
        });
    const SqlValue literal = has_ascii_alpha ? SqlValue::null()
                                             : SqlValue::text_value(text);

    static constexpr std::string_view kProjection =
        "SELECT m.id, m.timestamp, "
        "CASE WHEN " /* kHistoryEligibility */ "c.taskId IS NULL AND c.parentConversationId IS "
        "NULL AND m.participant IN ('USER', 'MODEL') AND m.text != '' AND substr(m.id, 1, 5) != "
        "'tool_' AND substr(m.id, 1, 7) != 'result_' AND substr(m.id, 1, 8) != 'compact_' THEN "
        "CASE WHEN ? IS NOT NULL THEN (instr(m.text, ?) > 0 OR instr(c.title, ?) > 0) ELSE "
        "(m.text LIKE '%' || ? || '%' ESCAPE '\\' OR c.title LIKE '%' || ? || '%' ESCAPE '\\') END "
        "ELSE 0 END AS directMatch, "
        "CASE WHEN " /* kHistoryEligibility */ "c.taskId IS NULL AND c.parentConversationId IS "
        "NULL AND m.participant IN ('USER', 'MODEL') AND m.text != '' AND substr(m.id, 1, 5) != "
        "'tool_' AND substr(m.id, 1, 7) != 'result_' AND substr(m.id, 1, 8) != 'compact_' "
        "AND m.participant = 'MODEL' "
        "AND instr(m.toolCallJson, '\"type\":\"citation\"') > 0 "
        "THEN m.toolCallJson ELSE NULL END AS toolCallJson "
        "FROM ( ";

    struct KeywordRow {
        std::string id;
        std::int64_t timestamp = 0;
        bool direct_match = false;
        std::optional<std::string> tool_call_json;
    };
    const auto page = [&](const std::optional<std::pair<std::int64_t, std::string>>& before,
                          int page_size) {
        std::string sql{kProjection};
        SqlArgs args;
        args.push_back(literal);
        args.push_back(literal);
        args.push_back(literal);
        // The LIKE arms bind the escaped pattern (escapeLikePattern), the literal arm the raw one.
        args.push_back(SqlValue::text_value(escaped));
        args.push_back(SqlValue::text_value(escaped));
        if (before.has_value()) {
            sql += "SELECT id, timestamp FROM messages WHERE (timestamp, id) < (?, ?) ";
            args.push_back(SqlValue::integer_value(before->first));
            args.push_back(SqlValue::text_value(before->second));
        } else {
            sql += "SELECT id, timestamp FROM messages ";
        }
        sql += "ORDER BY timestamp DESC, id DESC LIMIT ? ) page "
               "CROSS JOIN messages m ON m.id = page.id "
               "LEFT JOIN conversations c ON m.conversationId = c.id "
               "ORDER BY page.timestamp DESC, page.id DESC";
        args.push_back(SqlValue::integer_value(page_size));
        std::vector<KeywordRow> out;
        for (const SqlRow& row : sql_.query(sql, args)) {
            kotlin_check(row.size() == 4, "KeywordSearchRow: row shape mismatch");
            KeywordRow entry;
            entry.id = row[0].as_text();
            entry.timestamp = row[1].as_integer();
            entry.direct_match = row[2].as_integer() != 0;
            entry.tool_call_json = row[3].text_or_null();
            out.push_back(std::move(entry));
        }
        return out;
    };

    const int bounded_page_size = std::clamp(128, 1, 256);
    std::vector<std::string> ids;
    std::optional<std::pair<std::int64_t, std::string>> before;
    while (true) {
        const std::vector<KeywordRow> rows = page(before, bounded_page_size);
        if (rows.empty()) break;
        for (const KeywordRow& row : rows) {
            const bool advanced =
                !before.has_value() || row.timestamp < before->first ||
                (row.timestamp == before->first && row.id < before->second);
            kotlin_check(advanced, "Keyword search page did not advance its cursor");
            before = std::make_pair(row.timestamp, row.id);
            if (row.direct_match || citation_title_matches(row.tool_call_json, text)) {
                ids.push_back(row.id);
                if (static_cast<int>(ids.size()) == limit) break;
            }
        }
        if (static_cast<int>(ids.size()) == limit) break;
        if (static_cast<int>(rows.size()) < bounded_page_size) break;
    }
    if (ids.empty()) return {};
    std::vector<MessageEntity> out = get_searchable_messages_by_ids(ids);
    // citationSearchNewestFirst: timestamp DESC, id DESC.
    std::sort(out.begin(), out.end(), [](const MessageEntity& first, const MessageEntity& second) {
        if (first.timestamp != second.timestamp) return first.timestamp > second.timestamp;
        return first.id > second.id;
    });
    return out;
}

std::vector<MessageEntity> ChatSearchDao::get_messages_with_citation_segments_page(
    std::string_view after_id,
    int page_size) {
    const std::string sql =
        "SELECT m.* FROM messages m INNER JOIN conversations c ON m.conversationId = c.id "
        "WHERE c.taskId IS NULL AND c.parentConversationId IS NULL AND "
        "m.toolCallJson LIKE '%\"type\":\"citation\"%' AND "
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
    // Kotlin: ids-page cursor feeding either the semantic rows (bootstrap complete) or the legacy
    // message-eligibility rows during the one-time upgrade.
    if (limit <= 0) {
        throw KotlinError("IllegalArgumentException", "Failed requirement.");
    }
    const bool legacy_pending = !metadata_state_complete(sql_);
    const int physical_page_size = std::min(limit, 256);
    const std::string current_embedding =
        "e.generation = COALESCE((SELECT generation FROM semantic_index_ledger "
        "WHERE modelId = e.modelId), 0) "
        "AND NOT EXISTS (SELECT 1 FROM semantic_index_ledger "
        "WHERE modelId = e.modelId AND state = 'DELETED')";
    const auto rows_for = [&](const std::vector<std::int64_t>& ids, bool legacy) {
        std::string in;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (index != 0) in += ", ";
            in += "?";
        }
        SqlArgs args;
        for (const std::int64_t id : ids) args.push_back(SqlValue::integer_value(id));
        args.push_back(SqlValue::text_value(std::string(model_id)));
        std::string sql;
        if (!legacy) {
            sql = "SELECT e.id, e.messageId, e.embedding, e.dimension FROM embeddings e "
                  "CROSS JOIN semantic_sources s ON e.messageId = s.messageId "
                  "CROSS JOIN conversations c ON s.conversationId = c.id "
                  "WHERE e.id IN (" + in + ") AND e.modelId = ? AND " + current_embedding +
                  " AND c.taskId IS NULL AND c.parentConversationId IS NULL "
                  "AND s.historyEligible = 1 AND s.searchLength >= ? ORDER BY e.id";
            args.push_back(SqlValue::integer_value(minimum_text_length));
        } else {
            sql = "SELECT e.id, e.messageId, e.embedding, e.dimension FROM embeddings e "
                  "LEFT JOIN semantic_sources s ON s.messageId = e.messageId "
                  "LEFT JOIN messages m ON s.messageId IS NULL AND m.id = e.messageId "
                  "CROSS JOIN conversations c ON c.id = COALESCE(s.conversationId, m.conversationId) "
                  "WHERE e.id IN (" + in + ") AND e.modelId = ? AND " + current_embedding +
                  " AND c.taskId IS NULL AND c.parentConversationId IS NULL "
                  "AND CASE WHEN s.messageId IS NOT NULL "
                  "THEN s.historyEligible = 1 AND s.searchLength >= ? "
                  "ELSE (c.taskId IS NULL AND c.parentConversationId IS NULL "
                  "AND m.participant IN ('USER', 'MODEL') AND m.text != '' "
                  "AND substr(m.id, 1, 5) != 'tool_' AND substr(m.id, 1, 7) != 'result_' "
                  "AND substr(m.id, 1, 8) != 'compact_') AND LENGTH(m.text) >= ? END "
                  "ORDER BY e.id";
            args.push_back(SqlValue::integer_value(minimum_text_length));
            args.push_back(SqlValue::integer_value(minimum_text_length));
        }
        std::vector<EmbeddingSearchRow> out;
        for (const SqlRow& row : sql_.query(sql, args)) {
            kotlin_check(row.size() == 4, "EmbeddingSearchRow: row shape mismatch");
            EmbeddingSearchRow entry;
            entry.id = row[0].as_integer();
            entry.message_id = row[1].as_text();
            entry.embedding = row[2].as_blob();
            entry.dimension = static_cast<int>(row[3].as_integer());
            out.push_back(std::move(entry));
        }
        return out;
    };

    std::vector<EmbeddingSearchRow> out;
    std::int64_t cursor = after_id;
    while (static_cast<int>(out.size()) < limit) {
        std::vector<std::int64_t> ids;
        for (const SqlRow& row : sql_.query(
                 "SELECT id FROM embeddings WHERE modelId = ? AND id > ? ORDER BY id LIMIT ?",
                 {SqlValue::text_value(std::string(model_id)), SqlValue::integer_value(cursor),
                  SqlValue::integer_value(physical_page_size)})) {
            ids.push_back(row[0].as_integer());
        }
        if (ids.empty()) break;
        const std::vector<EmbeddingSearchRow> page = rows_for(ids, legacy_pending);
        const int missing = limit - static_cast<int>(out.size());
        out.insert(out.end(), page.begin(),
                   page.begin() + std::min<std::ptrdiff_t>(missing, page.size()));
        cursor = ids.back();
    }
    return out;
}

int ChatSearchDao::get_embedding_count_by_model(std::string_view model_id) {
    const CacheCounts counts = embedding_cache_counts(sql_, {std::string(model_id)});
    kotlin_check(!counts.per_model.empty(), "getEmbeddingCacheCounts: requested model missing");
    return counts.per_model.front().second;
}

std::vector<EmbeddingModelCount> ChatSearchDao::get_embedding_counts_by_models(
    const std::vector<std::string>& model_ids) {
    // Kotlin guards the empty list before touching the database.
    if (model_ids.empty()) return {};
    const CacheCounts counts = embedding_cache_counts(sql_, model_ids);
    std::vector<EmbeddingModelCount> out;
    for (const auto& [model_id, count] : counts.per_model) {
        if (count > 0) out.push_back(EmbeddingModelCount{model_id, count});
    }
    return out;
}

int ChatSearchDao::get_indexable_message_count() {
    return embedding_cache_counts(sql_, {}).total;
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
        ") AND c.taskId IS NULL AND c.parentConversationId IS NULL AND "
        "m.participant IN ('USER', 'MODEL') AND m.text != '' AND substr(m.id, 1, 5) != "
        "'tool_' AND substr(m.id, 1, 7) != 'result_' AND substr(m.id, 1, 8) != 'compact_'";
    const std::vector<SqlRow> rows = sql_.query(sql, text_list(ids));
    std::vector<MessageEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(message_from_row(row));
    return out;
}

bool ChatSearchDao::is_message_searchable(std::string_view message_id) {
    const std::string sql =
        // Kotlin keeps the LIKE spelling here (not the substr spelling of HISTORY): the wildcard
        // also hides ids like "resultx"/"toolx" that merely start with the word.
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
        "SELECT * FROM conversations WHERE id = ? AND taskId IS NULL AND "
        "parentConversationId IS NULL";
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    return conversation_from_row(rows.front());
}

int ChatSearchDao::get_searchable_conversation_count() {
    return single_count(sql_.query(
        "SELECT COUNT(*) FROM conversations WHERE taskId IS NULL AND parentConversationId IS NULL",
        {}));
}

std::vector<ChatEntity> ChatSearchDao::get_searchable_conversations_page_ascending(int offset,
                                                                                   int limit) {
    const std::string sql =
        "SELECT * FROM conversations WHERE taskId IS NULL AND parentConversationId IS NULL "
        "ORDER BY lastUpdated ASC, id ASC LIMIT ? OFFSET ?";
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
        "SELECT * FROM conversations WHERE taskId IS NULL AND parentConversationId IS NULL "
        "ORDER BY lastUpdated DESC, id DESC LIMIT ? OFFSET ?";
    const std::vector<SqlRow> rows =
        sql_.query(sql, {SqlValue::integer_value(limit), SqlValue::integer_value(offset)});
    std::vector<ChatEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(conversation_from_row(row));
    return out;
}

}  // namespace agora::core::chat_search
