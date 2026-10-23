// Port of data/local/ChatContextCompactDao.kt.
//
// The compact-graph contract: a Recompact swaps a terminal Compact message's dedicated Run for a
// fresh active Run while the message keeps its identity and graph position, and a Remove folds
// the Compact out of both branch projections before deleting its row. Both are @Transaction
// bodies transliterated here, including every `check`/`require`/`error` message; the plain
// members carry the @Query text verbatim.
#include "agora/core/context_compact.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::context_compact {

namespace {

namespace conversation = agora::core::conversation_runtime;
namespace json = agora::core::json;
namespace recovery = agora::core::run_recovery;

constexpr std::string_view kCompactPrefix = "compact_";

// ---- wire conversions (Room maps enum columns with Enum.valueOf) ---------------------------

conversation::RunStatus run_status_from_wire(const std::string& wire) {
    if (wire == "ACTIVE") return conversation::RunStatus::Active;
    if (wire == "STOPPING") return conversation::RunStatus::Stopping;
    if (wire == "COMPLETED") return conversation::RunStatus::Completed;
    if (wire == "STOPPED") return conversation::RunStatus::Stopped;
    if (wire == "FAILED") return conversation::RunStatus::Failed;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.RunStatus." + wire);
}

std::string run_status_to_wire(conversation::RunStatus status) {
    switch (status) {
        case conversation::RunStatus::Active:
            return "ACTIVE";
        case conversation::RunStatus::Stopping:
            return "STOPPING";
        case conversation::RunStatus::Completed:
            return "COMPLETED";
        case conversation::RunStatus::Stopped:
            return "STOPPED";
        case conversation::RunStatus::Failed:
            return "FAILED";
    }
    throw KotlinError("IllegalArgumentException", "unmapped RunStatus");
}

std::optional<conversation::RunEndReason> run_end_reason_from_wire(
    const std::optional<std::string>& wire) {
    if (!wire.has_value()) return std::nullopt;
    if (*wire == "MODEL_COMPLETED") return conversation::RunEndReason::ModelCompleted;
    if (*wire == "USER_STOPPED") return conversation::RunEndReason::UserStopped;
    if (*wire == "PROCESS_RECOVERED") return conversation::RunEndReason::ProcessRecovered;
    if (*wire == "PROVIDER_ERROR") return conversation::RunEndReason::ProviderError;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.RunEndReason." + *wire);
}

std::string run_end_reason_to_wire(conversation::RunEndReason reason) {
    switch (reason) {
        case conversation::RunEndReason::ModelCompleted:
            return "MODEL_COMPLETED";
        case conversation::RunEndReason::UserStopped:
            return "USER_STOPPED";
        case conversation::RunEndReason::ProcessRecovered:
            return "PROCESS_RECOVERED";
        case conversation::RunEndReason::ProviderError:
            return "PROVIDER_ERROR";
    }
    throw KotlinError("IllegalArgumentException", "unmapped RunEndReason");
}

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

std::string message_status_to_wire(recovery::MessageStatus status) {
    switch (status) {
        case recovery::MessageStatus::transcribing:
            return "TRANSCRIBING";
        case recovery::MessageStatus::sending:
            return "SENDING";
        case recovery::MessageStatus::thinking:
            return "THINKING";
        case recovery::MessageStatus::tool_calling:
            return "TOOL_CALLING";
        case recovery::MessageStatus::success:
            return "SUCCESS";
        case recovery::MessageStatus::stopped:
            return "STOPPED";
        case recovery::MessageStatus::error:
            return "ERROR";
    }
    throw KotlinError("IllegalArgumentException", "unmapped MessageStatus");
}

recovery::Participant participant_from_wire(const std::string& wire) {
    if (wire == "USER") return recovery::Participant::user;
    if (wire == "MODEL") return recovery::Participant::model;
    if (wire == "ERROR") return recovery::Participant::error;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.Participant." + wire);
}

std::string participant_to_wire(recovery::Participant participant) {
    switch (participant) {
        case recovery::Participant::user:
            return "USER";
        case recovery::Participant::model:
            return "MODEL";
        case recovery::Participant::error:
            return "ERROR";
    }
    throw KotlinError("IllegalArgumentException", "unmapped Participant");
}

// ---- MessageConverters ---------------------------------------------------------------------

/** MessageConverters.toStringList: JSON array, with the legacy "|||" delimiter accepted. */
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

/** MessageConverters.fromStringList: the JSON array encoding of a list. */
std::string encode_string_list(const std::vector<std::string>& values) {
    json::Value array;
    array.kind = json::Kind::Array;
    for (const std::string& value : values) {
        json::Value element;
        element.kind = json::Kind::String;
        element.text = value;
        array.elements.push_back(std::move(element));
    }
    return json::to_kotlin_string(array);
}

// ---- row mapping ----------------------------------------------------------------------------

SqlValue optional_text(const std::optional<std::string>& value) {
    return value.has_value() ? SqlValue::text_value(*value) : SqlValue::null();
}

SqlValue optional_int(const std::optional<int>& value) {
    return value.has_value() ? SqlValue::integer_value(*value) : SqlValue::null();
}

SqlValue optional_long(const std::optional<std::int64_t>& value) {
    return value.has_value() ? SqlValue::integer_value(*value) : SqlValue::null();
}

std::optional<int> cell_int(const SqlValue& cell) {
    const std::optional<std::int64_t> value = cell.integer_or_null();
    if (!value.has_value()) return std::nullopt;
    return static_cast<int>(*value);
}

MessageEntity message_from_row(const SqlRow& row) {
    // `SELECT * FROM messages` over the stand schema, in declaration/column order.
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

RunEntity run_from_row(const SqlRow& row) {
    // Enum columns resolve first (Enum.valueOf), then the init block runs — the Kotlin order.
    kotlin_check(row.size() == 12, "RunEntity: row shape mismatch");
    RunEntity run;
    run.id = row[0].as_text();
    run.conversation_id = row[1].as_text();
    run.parent_run_id = row[2].text_or_null();
    run.status = run_status_from_wire(row[3].as_text());
    run.active_slot = row[4].integer_or_null();
    run.started_at = row[5].as_integer();
    run.last_checkpoint_at = row[6].as_integer();
    run.stop_requested_at = row[7].integer_or_null();
    run.ended_at = row[8].integer_or_null();
    run.end_reason = run_end_reason_from_wire(row[9].text_or_null());
    run.current_pass = static_cast<int>(row[10].as_integer());
    run.legacy_ambiguous = row[11].as_integer() != 0;
    run.validate();
    return run;
}

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

bool is_terminal(const RunEntity& run) {
    return run.status != conversation::RunStatus::Active &&
           run.status != conversation::RunStatus::Stopping;
}

/** withTransaction: the block marks the level successful unless it throws. */
template <typename Body>
auto run_in_transaction(ContextCompactDao& dao, Body&& body) -> decltype(body()) {
    dao.begin_transaction();
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

}  // namespace

// ---- entity validation ---------------------------------------------------------------------

void RunEntity::validate() const {
    // RunEntity's init block, in source order.
    kotlin_require(current_pass >= 0);
    const bool terminal = is_terminal(*this);
    kotlin_require((active_slot.has_value() && *active_slot == 1) == !terminal);
    kotlin_require(terminal == (ended_at.has_value() && end_reason.has_value()));
    kotlin_require(status != conversation::RunStatus::Stopping || stop_requested_at.has_value());
}

// ---- selection map helpers -----------------------------------------------------------------

SelectionMap decode_compact_selections(const std::optional<std::string>& raw) {
    // `raw?.let { runCatching { decode... }.getOrDefault(emptyMap()) } ?: emptyMap()`
    if (!raw.has_value()) return {};
    return run_branch_selection::decode_selection_map(*raw).value_or(SelectionMap{});
}

std::string encode_compact_selections(const SelectionMap& selections) {
    return run_branch_selection::encode_selection_map(selections);
}

std::optional<std::string> selection_get(const SelectionMap& selections,
                                         const std::optional<std::string>& key) {
    for (const auto& [entry_key, value] : selections) {
        if (entry_key == key) return value;
    }
    return std::nullopt;
}

std::optional<std::string> selection_remove(SelectionMap& selections,
                                            const std::optional<std::string>& key) {
    for (auto it = selections.begin(); it != selections.end(); ++it) {
        if (it->first == key) {
            std::string value = std::move(it->second);
            selections.erase(it);
            return value;
        }
    }
    return std::nullopt;
}

void selection_put(SelectionMap& selections, std::optional<std::string> key, std::string value) {
    for (auto& entry : selections) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    selections.emplace_back(std::move(key), std::move(value));
}

bool selections_equal(const SelectionMap& left, const SelectionMap& right) {
    // Kotlin Map.equals is order independent; keys are unique within a decoded map.
    if (left.size() != right.size()) return false;
    for (const auto& [key, value] : left) {
        const std::optional<std::string> other = selection_get(right, key);
        if (!other.has_value() || *other != value) return false;
    }
    return true;
}

// ---- transaction surface -------------------------------------------------------------------

void ContextCompactDao::begin_transaction() { stack_.begin(); }

void ContextCompactDao::set_transaction_successful() { stack_.mark_successful(); }

void ContextCompactDao::end_transaction() { stack_.end(); }

// ---- plain members -------------------------------------------------------------------------

std::optional<ConversationEntry> ContextCompactDao::get_conversation(
    std::string_view conversation_id) {
    std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM conversations WHERE id = ?",
        {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    kotlin_check(rows.front().size() >= 3, "ChatEntity: row shape mismatch");
    ConversationEntry entry;
    entry.id = rows.front()[0].as_text();
    entry.selected_branches_json = rows.front()[1].text_or_null();
    entry.selected_run_branches_json = rows.front()[2].text_or_null();
    return entry;
}

void ContextCompactDao::insert_message(const MessageEntity& message) {
    static constexpr std::string_view kSql =
        "INSERT INTO messages (id, conversationId, parentId, text, images, thoughts, thoughtTitle, "
        "tokenCount, inputTokenCount, cachedInputTokenCount, cacheWriteInputTokenCount, "
        "uncachedInputTokenCount, outputTokenCount, reasoningTokenCount, generationDurationMs, "
        "status, participant, timestamp, thoughtTimeMs, modelName, toolCallJson, attachmentMeta, "
        "runId, runSequence, consumedAtPass, sourceJson) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    SqlArgs args;
    args.push_back(SqlValue::text_value(message.id));
    args.push_back(SqlValue::text_value(message.conversation_id));
    args.push_back(optional_text(message.parent_id));
    args.push_back(SqlValue::text_value(message.text));
    args.push_back(SqlValue::text_value(encode_string_list(message.images)));
    args.push_back(optional_text(message.thoughts));
    args.push_back(optional_text(message.thought_title));
    args.push_back(SqlValue::integer_value(message.token_count));
    args.push_back(optional_int(message.input_token_count));
    args.push_back(optional_int(message.cached_input_token_count));
    args.push_back(optional_int(message.cache_write_input_token_count));
    args.push_back(optional_int(message.uncached_input_token_count));
    args.push_back(optional_int(message.output_token_count));
    args.push_back(optional_int(message.reasoning_token_count));
    args.push_back(optional_long(message.generation_duration_ms));
    args.push_back(SqlValue::text_value(message_status_to_wire(message.status)));
    args.push_back(SqlValue::text_value(participant_to_wire(message.participant)));
    args.push_back(SqlValue::integer_value(message.timestamp));
    args.push_back(optional_long(message.thought_time_ms));
    args.push_back(optional_text(message.model_name));
    args.push_back(optional_text(message.tool_call_json));
    args.push_back(optional_text(message.attachment_meta));
    args.push_back(SqlValue::text_value(message.run_id));
    args.push_back(SqlValue::integer_value(message.run_sequence));
    args.push_back(optional_int(message.consumed_at_pass));
    args.push_back(optional_text(message.source_json));
    sql_.execute(std::string(kSql), args);
}

std::optional<MessageEntity> ContextCompactDao::get_message(std::string_view message_id) {
    std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM messages WHERE id = ?",
        {SqlValue::text_value(std::string(message_id))});
    if (rows.empty()) return std::nullopt;
    return message_from_row(rows.front());
}

void ContextCompactDao::insert_run(const RunEntity& run) {
    run.validate();
    static constexpr std::string_view kSql =
        "INSERT INTO runs (id, conversationId, parentRunId, status, activeSlot, startedAt, "
        "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, legacyAmbiguous) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sql_.execute(
        std::string(kSql),
        {SqlValue::text_value(run.id), SqlValue::text_value(run.conversation_id),
         optional_text(run.parent_run_id), SqlValue::text_value(run_status_to_wire(run.status)),
         optional_long(run.active_slot), SqlValue::integer_value(run.started_at),
         SqlValue::integer_value(run.last_checkpoint_at), optional_long(run.stop_requested_at),
         optional_long(run.ended_at),
         run.end_reason.has_value()
             ? SqlValue::text_value(run_end_reason_to_wire(*run.end_reason))
             : SqlValue::null(),
         SqlValue::integer_value(run.current_pass),
         SqlValue::integer_value(run.legacy_ambiguous ? 1 : 0)});
}

std::optional<RunEntity> ContextCompactDao::get_run(std::string_view run_id) {
    std::vector<SqlRow> rows = sql_.query("SELECT * FROM runs WHERE id = ?",
                                          {SqlValue::text_value(std::string(run_id))});
    if (rows.empty()) return std::nullopt;
    return run_from_row(rows.front());
}

std::vector<MessageEntity> ContextCompactDao::get_messages_for_runs(
    const std::vector<std::string>& run_ids) {
    if (run_ids.empty()) return {};
    const std::string sql = "SELECT * FROM messages WHERE runId IN (" + placeholders(run_ids.size()) +
                            ") ORDER BY runSequence, timestamp, id";
    std::vector<MessageEntity> out;
    for (const SqlRow& row : sql_.query(sql, text_list(run_ids))) {
        out.push_back(message_from_row(row));
    }
    return out;
}

std::int64_t ContextCompactDao::delete_run(std::string_view run_id) {
    return sql_.execute("DELETE FROM runs WHERE id = ?",
                        {SqlValue::text_value(std::string(run_id))});
}

void ContextCompactDao::delete_embeddings_by_message_ids(
    const std::vector<std::string>& message_ids) {
    if (message_ids.empty()) return;
    sql_.execute("DELETE FROM embeddings WHERE messageId IN (" + placeholders(message_ids.size()) +
                     ")",
                 text_list(message_ids));
}

std::int64_t ContextCompactDao::update_selections_for_run_deletion(
    std::string_view conversation_id, std::string_view selected_branches_json,
    std::string_view selected_run_branches_json, std::int64_t at) {
    static constexpr std::string_view kSql =
        "UPDATE conversations SET selectedBranchesJson = ?, selectedRunBranchesJson = ?, "
        "lastUpdated = ?, dataChangedAt = ? WHERE id = ?";
    return sql_.execute(
        std::string(kSql),
        {SqlValue::text_value(std::string(selected_branches_json)),
         SqlValue::text_value(std::string(selected_run_branches_json)),
         SqlValue::integer_value(at), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(conversation_id))});
}

std::int64_t ContextCompactDao::update_message_branch_selections(
    std::string_view conversation_id, std::string_view selected_branches_json,
    std::int64_t at) {
    static constexpr std::string_view kSql =
        "UPDATE conversations SET selectedBranchesJson = ?, dataChangedAt = ? WHERE id = ?";
    return sql_.execute(std::string(kSql),
                        {SqlValue::text_value(std::string(selected_branches_json)),
                         SqlValue::integer_value(at),
                         SqlValue::text_value(std::string(conversation_id))});
}

std::int64_t ContextCompactDao::update_run_branch_selections(
    std::string_view conversation_id, std::string_view selected_run_branches_json,
    std::int64_t at) {
    static constexpr std::string_view kSql =
        "UPDATE conversations SET selectedRunBranchesJson = ?, dataChangedAt = ? WHERE id = ?";
    return sql_.execute(std::string(kSql),
                        {SqlValue::text_value(std::string(selected_run_branches_json)),
                         SqlValue::integer_value(at),
                         SqlValue::text_value(std::string(conversation_id))});
}

std::int64_t ContextCompactDao::update_branch_selections(
    std::string_view conversation_id, std::string_view selected_branches_json,
    std::string_view selected_run_branches_json, std::int64_t at) {
    static constexpr std::string_view kSql =
        "UPDATE conversations SET selectedBranchesJson = ?, selectedRunBranchesJson = ?, "
        "dataChangedAt = ? WHERE id = ?";
    return sql_.execute(
        std::string(kSql),
        {SqlValue::text_value(std::string(selected_branches_json)),
         SqlValue::text_value(std::string(selected_run_branches_json)),
         SqlValue::integer_value(at), SqlValue::text_value(std::string(conversation_id))});
}

std::optional<RunEntity> ContextCompactDao::get_live_run(std::string_view conversation_id) {
    std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM runs WHERE conversationId = ? AND activeSlot = 1 LIMIT 1",
        {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    return run_from_row(rows.front());
}

std::int64_t ContextCompactDao::next_run_sequence(std::string_view run_id) {
    std::vector<SqlRow> rows = sql_.query(
        "SELECT COALESCE(MAX(runSequence), -1) + 1 FROM messages WHERE runId = ?",
        {SqlValue::text_value(std::string(run_id))});
    kotlin_check(!rows.empty() && rows.front().size() == 1,
                 "ContextCompactDao: nextRunSequence returned no row");
    return rows.front().front().as_integer();
}

std::int64_t ContextCompactDao::touch_run(std::string_view run_id, std::int64_t at) {
    return sql_.execute("UPDATE runs SET lastCheckpointAt = ? WHERE id = ?",
                        {SqlValue::integer_value(at),
                         SqlValue::text_value(std::string(run_id))});
}

void ContextCompactDao::delete_messages_by_ids(const std::vector<std::string>& ids) {
    if (ids.empty()) return;
    sql_.execute("DELETE FROM messages WHERE id IN (" + placeholders(ids.size()) + ")",
                 text_list(ids));
}

std::int64_t ContextCompactDao::reparent_message_children(
    std::string_view removed_message_id, const std::optional<std::string>& replacement_parent_id) {
    static constexpr std::string_view kSql =
        "UPDATE messages SET parentId = ? WHERE parentId = ?";
    return sql_.execute(std::string(kSql),
                        {optional_text(replacement_parent_id),
                         SqlValue::text_value(std::string(removed_message_id))});
}

std::int64_t ContextCompactDao::reparent_run_children(
    std::string_view removed_run_id,
    const std::optional<std::string>& replacement_parent_run_id) {
    static constexpr std::string_view kSql = "UPDATE runs SET parentRunId = ? WHERE parentRunId = ?";
    return sql_.execute(std::string(kSql),
                        {optional_text(replacement_parent_run_id),
                         SqlValue::text_value(std::string(removed_run_id))});
}

std::int64_t ContextCompactDao::replace_context_compact_message_run(
    std::string_view message_id, std::string_view old_run_id, std::string_view new_run_id,
    std::string_view model_name) {
    static constexpr std::string_view kSql =
        "UPDATE messages SET runId = ?, runSequence = 0, text = '', status = 'SENDING', "
        "modelName = ? WHERE id = ? AND runId = ? AND status IN ('SUCCESS', 'ERROR', 'STOPPED')";
    return sql_.execute(std::string(kSql),
                        {SqlValue::text_value(std::string(new_run_id)),
                         SqlValue::text_value(std::string(model_name)),
                         SqlValue::text_value(std::string(message_id)),
                         SqlValue::text_value(std::string(old_run_id))});
}

// ---- @Transaction bodies -------------------------------------------------------------------

MessageEntity ContextCompactDao::begin_recompact_context_compact(
    const RunEntity& replacement_run, std::string_view message_id, std::string_view model_name,
    std::string_view expected_selected_branches_json) {
    return run_in_transaction(*this, [&] {
        const std::optional<MessageEntity> message = get_message(message_id);
        if (!message.has_value()) {
            throw KotlinError("IllegalStateException", "Compact disappeared");
        }
        kotlin_check(!get_live_run(message->conversation_id).has_value(),
                     "Conversation became busy during Recompact");
        kotlin_require(message->id.rfind(kCompactPrefix, 0) == 0);
        const std::optional<RunEntity> old_run = get_run(message->run_id);
        if (!old_run.has_value()) {
            throw KotlinError("IllegalStateException", "Compact Run disappeared");
        }
        kotlin_check(is_terminal(*old_run) && !old_run->active_slot.has_value(),
                     "Compact Run is not terminal");
        std::vector<std::string> run_message_ids;
        for (const MessageEntity& run_message : get_messages_for_runs({old_run->id})) {
            run_message_ids.push_back(run_message.id);
        }
        kotlin_check(run_message_ids == std::vector<std::string>{message->id},
                     "Compact Run is not independently replaceable");
        kotlin_check(message->status == recovery::MessageStatus::success ||
                         message->status == recovery::MessageStatus::error ||
                         message->status == recovery::MessageStatus::stopped,
                     "Compact message is not terminal");

        const std::optional<ConversationEntry> conversation =
            get_conversation(message->conversation_id);
        if (!conversation.has_value()) {
            throw KotlinError("IllegalStateException",
                              "Conversation " + message->conversation_id + " disappeared");
        }
        kotlin_check(
            selections_equal(
                decode_compact_selections(conversation->selected_branches_json),
                decode_compact_selections(std::string(expected_selected_branches_json))),
            "Selected message branch changed before Recompact");

        if (!message->parent_id.has_value()) {
            throw KotlinError("IllegalStateException", "Compact parent disappeared");
        }
        const std::optional<MessageEntity> parent = get_message(*message->parent_id);
        if (!parent.has_value()) {
            throw KotlinError("IllegalStateException", "Compact parent disappeared");
        }

        kotlin_require(replacement_run.id != old_run->id, "Recompact requires a fresh Run");
        kotlin_require(replacement_run.conversation_id == message->conversation_id);
        kotlin_require(replacement_run.status == conversation::RunStatus::Active &&
                           replacement_run.active_slot == 1);
        kotlin_require(replacement_run.parent_run_id.has_value() &&
                           *replacement_run.parent_run_id == parent->run_id);
        kotlin_check(old_run->parent_run_id.has_value() &&
                         *old_run->parent_run_id == parent->run_id,
                     "Compact Run ancestry changed");

        SelectionMap run_selections =
            decode_compact_selections(conversation->selected_run_branches_json);
        for (const auto& [parent_run_id, selected_run_id] : run_selections) {
            if (selected_run_id == old_run->id && parent_run_id != old_run->parent_run_id) {
                kotlin_check(false, "Selected Run graph is inconsistent");
            }
        }
        const std::optional<std::string> selected_child_run =
            selection_remove(run_selections, old_run->id);
        if (selection_get(run_selections, old_run->parent_run_id) == old_run->id) {
            selection_put(run_selections, old_run->parent_run_id, replacement_run.id);
        }
        if (selected_child_run.has_value()) {
            selection_put(run_selections, replacement_run.id, *selected_child_run);
        }

        insert_run(replacement_run);
        kotlin_check(replace_context_compact_message_run(
                         message->id, old_run->id, replacement_run.id, model_name) == 1,
                     "Check failed.");
        reparent_run_children(old_run->id, replacement_run.id);
        kotlin_check(update_selections_for_run_deletion(
                         message->conversation_id,
                         conversation->selected_branches_json.value_or("{}"),
                         encode_compact_selections(run_selections),
                         replacement_run.started_at) == 1,
                     "Check failed.");
        kotlin_check(delete_run(old_run->id) == 1, "Check failed.");

        MessageEntity updated = *message;
        updated.run_id = replacement_run.id;
        updated.run_sequence = 0;
        updated.text.clear();
        updated.status = recovery::MessageStatus::sending;
        updated.model_name = std::string(model_name);
        return updated;
    });
}

bool ContextCompactDao::remove_context_compact(std::string_view message_id,
                                               std::int64_t now_ms) {
    return run_in_transaction(*this, [&] {
        const std::optional<MessageEntity> message = get_message(message_id);
        if (!message.has_value()) return false;
        kotlin_require(message->id.rfind(kCompactPrefix, 0) == 0);
        const std::optional<ConversationEntry> conversation =
            get_conversation(message->conversation_id);
        if (!conversation.has_value()) return false;
        const std::optional<RunEntity> compact_run = get_run(message->run_id);
        if (!compact_run.has_value()) return false;

        std::vector<std::string> run_message_ids;
        for (const MessageEntity& run_message : get_messages_for_runs({compact_run->id})) {
            run_message_ids.push_back(run_message.id);
        }
        const bool compact_owns_dedicated_run =
            run_message_ids == std::vector<std::string>{message->id};

        reparent_message_children(message->id, message->parent_id);
        delete_embeddings_by_message_ids({message->id});
        delete_messages_by_ids({message->id});

        SelectionMap selections =
            decode_compact_selections(conversation->selected_branches_json);
        const bool selected_compact =
            selection_get(selections, message->parent_id) == message->id;
        const std::optional<std::string> selected_suffix_child_id =
            selection_remove(selections, message->id);
        if (selected_compact) {
            if (!selected_suffix_child_id.has_value()) {
                selection_remove(selections, message->parent_id);
            } else {
                selection_put(selections, message->parent_id, *selected_suffix_child_id);
            }
        }

        SelectionMap run_selections =
            decode_compact_selections(conversation->selected_run_branches_json);
        if (compact_owns_dedicated_run) {
            const bool selected_compact_run =
                selection_get(run_selections, compact_run->parent_run_id) == compact_run->id;
            const std::optional<std::string> selected_compact_run_child =
                selection_remove(run_selections, compact_run->id);
            if (selected_compact_run) {
                if (!selected_compact_run_child.has_value()) {
                    selection_remove(run_selections, compact_run->parent_run_id);
                } else {
                    selection_put(run_selections, compact_run->parent_run_id,
                                  *selected_compact_run_child);
                }
            }
        }

        kotlin_check(update_selections_for_run_deletion(
                         message->conversation_id, encode_compact_selections(selections),
                         encode_compact_selections(run_selections), now_ms) == 1,
                     "Check failed.");
        if (compact_owns_dedicated_run) {
            reparent_run_children(compact_run->id, compact_run->parent_run_id);
            kotlin_check(delete_run(compact_run->id) == 1, "Check failed.");
        }
        return true;
    });
}

}  // namespace agora::core::context_compact
