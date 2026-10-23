// Port of data/local/ChatDao.kt.
//
// ChatDao is the single Room DAO in the app: its own declarations plus the six inherited DAO
// interfaces. The inherited surfaces are composed by public inheritance from their ports; this
// file contains ChatDao's SQL text and the transliteration of every @Transaction body.
//
// Room is replaced by the SqlExecutor boundary. Named placeholders are bound in the order they
// appear in the SQL text. @Upsert is INSERT followed by UPDATE-by-primary-key on a constraint
// failure. Observed queries return their current rows as the Flow's initial emission.
#include "agora/core/chat_dao.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/backup_media_policy.hpp"
#include "agora/core/json.hpp"

namespace agora::core::chat_dao {

namespace {

namespace json = agora::core::json;
namespace backup = agora::core::backup;
namespace compact = agora::core::context_compact;
namespace recovery = agora::core::run_recovery;
namespace runtime = agora::core::conversation_runtime;
namespace selection = agora::core::run_branch_selection;
namespace automation = agora::core::automation;
namespace new_chat = agora::core::new_chat_persist;

// ---- wire conversions (Room maps enum columns with Enum.valueOf) ---------------------------

std::string message_status_to_wire(recovery::MessageStatus status) {
    switch (status) {
        case recovery::MessageStatus::transcribing: return "TRANSCRIBING";
        case recovery::MessageStatus::sending: return "SENDING";
        case recovery::MessageStatus::thinking: return "THINKING";
        case recovery::MessageStatus::tool_calling: return "TOOL_CALLING";
        case recovery::MessageStatus::success: return "SUCCESS";
        case recovery::MessageStatus::stopped: return "STOPPED";
        case recovery::MessageStatus::error: return "ERROR";
    }
    throw KotlinError("IllegalArgumentException", "Unknown MessageStatus");
}

recovery::MessageStatus message_status_from_wire(const std::string& wire) {
    if (wire == "TRANSCRIBING") return recovery::MessageStatus::transcribing;
    if (wire == "SENDING") return recovery::MessageStatus::sending;
    if (wire == "THINKING") return recovery::MessageStatus::thinking;
    if (wire == "TOOL_CALLING") return recovery::MessageStatus::tool_calling;
    if (wire == "SUCCESS") return recovery::MessageStatus::success;
    if (wire == "STOPPED") return recovery::MessageStatus::stopped;
    if (wire == "ERROR") return recovery::MessageStatus::error;
    throw KotlinError("IllegalArgumentException", "Unknown MessageStatus " + wire);
}

std::string participant_to_wire(recovery::Participant participant) {
    switch (participant) {
        case recovery::Participant::user: return "USER";
        case recovery::Participant::model: return "MODEL";
        case recovery::Participant::error: return "ERROR";
    }
    throw KotlinError("IllegalArgumentException", "Unknown Participant");
}

recovery::Participant participant_from_wire(const std::string& wire) {
    if (wire == "USER") return recovery::Participant::user;
    if (wire == "MODEL") return recovery::Participant::model;
    if (wire == "ERROR") return recovery::Participant::error;
    throw KotlinError("IllegalArgumentException", "Unknown Participant " + wire);
}

std::string run_status_to_wire(runtime::RunStatus status) {
    switch (status) {
        case runtime::RunStatus::Active: return "ACTIVE";
        case runtime::RunStatus::Stopping: return "STOPPING";
        case runtime::RunStatus::Completed: return "COMPLETED";
        case runtime::RunStatus::Stopped: return "STOPPED";
        case runtime::RunStatus::Failed: return "FAILED";
    }
    throw KotlinError("IllegalArgumentException", "Unknown RunStatus");
}

runtime::RunStatus run_status_from_wire(const std::string& wire) {
    if (wire == "ACTIVE") return runtime::RunStatus::Active;
    if (wire == "STOPPING") return runtime::RunStatus::Stopping;
    if (wire == "COMPLETED") return runtime::RunStatus::Completed;
    if (wire == "STOPPED") return runtime::RunStatus::Stopped;
    if (wire == "FAILED") return runtime::RunStatus::Failed;
    throw KotlinError("IllegalArgumentException", "Unknown RunStatus " + wire);
}

std::string run_end_reason_to_wire(runtime::RunEndReason reason) {
    switch (reason) {
        case runtime::RunEndReason::ModelCompleted: return "MODEL_COMPLETED";
        case runtime::RunEndReason::UserStopped: return "USER_STOPPED";
        case runtime::RunEndReason::ProcessRecovered: return "PROCESS_RECOVERED";
        case runtime::RunEndReason::ProviderError: return "PROVIDER_ERROR";
    }
    throw KotlinError("IllegalArgumentException", "Unknown RunEndReason");
}

runtime::RunEndReason run_end_reason_from_wire(const std::string& wire) {
    if (wire == "MODEL_COMPLETED") return runtime::RunEndReason::ModelCompleted;
    if (wire == "USER_STOPPED") return runtime::RunEndReason::UserStopped;
    if (wire == "PROCESS_RECOVERED") return runtime::RunEndReason::ProcessRecovered;
    if (wire == "PROVIDER_ERROR") return runtime::RunEndReason::ProviderError;
    throw KotlinError("IllegalArgumentException", "Unknown RunEndReason " + wire);
}

// ---- SQL values / rows ---------------------------------------------------------------------

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

std::vector<std::string> decode_string_list(const std::string& raw) {
    if (raw.empty()) return {};
    const json::ParseResult parsed = json::parse(raw);
    if (parsed.value.has_value() && parsed.value->kind == json::Kind::Array) {
        std::vector<std::string> out;
        bool all_strings = true;
        for (const json::Value& element : parsed.value->elements) {
            if (element.kind != json::Kind::String) {
                all_strings = false;
                break;
            }
            out.push_back(element.text);
        }
        if (all_strings) return out;
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

/** The IN (...) placeholder list; an empty collection binds NULL (Room's expansion shape). */
std::string placeholders(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (!out.empty()) out += ", ";
        out += "?";
    }
    if (out.empty()) out = "NULL";
    return out;
}

SqlArgs text_args(const std::vector<std::string>& values) {
    SqlArgs args;
    args.reserve(values.size());
    for (const std::string& value : values) args.push_back(SqlValue::text_value(value));
    return args;
}

// ---- row mapping ---------------------------------------------------------------------------

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

ChatEntity chat_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 15, "ChatEntity: row shape mismatch");
    ChatEntity chat;
    chat.id = row[0].as_text();
    chat.title = row[1].as_text();
    chat.last_updated = row[2].as_integer();
    chat.data_changed_at = row[3].as_integer();
    chat.selected_branches_json = row[4].text_or_null();
    chat.system_prompt_id = row[5].text_or_null();
    chat.model_id = row[6].text_or_null();
    chat.task_id = row[7].text_or_null();
    chat.origin = row[8].as_text();
    chat.graduated = row[9].as_integer() != 0;
    chat.draft_text = row[10].as_text();
    chat.draft_attachments = row[11].text_or_null();
    chat.selected_run_branches_json = row[12].text_or_null();
    chat.has_unread_generation = row[13].as_integer() != 0;
    chat.is_pinned = row[14].as_integer() != 0;
    return chat;
}

ChatConversation chat_conversation_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 10, "ChatConversation: row shape mismatch");
    ChatConversation projection;
    projection.id = row[0].as_text();
    projection.title = row[1].as_text();
    projection.system_prompt_id = row[2].text_or_null();
    projection.model_id = row[3].text_or_null();
    projection.task_id = row[4].text_or_null();
    projection.origin = row[5].as_text();
    projection.graduated = row[6].as_integer() != 0;
    projection.has_unread_generation = row[7].as_integer() != 0;
    projection.is_pinned = row[8].as_integer() != 0;
    projection.selected_branches_json = row[9].text_or_null();
    return projection;
}

DrawerConversationRow drawer_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 4, "DrawerConversationRow: row shape mismatch");
    return DrawerConversationRow{row[0].as_text(), row[1].as_text(),
                                 row[2].as_integer() != 0, row[3].as_integer() != 0};
}

ConversationSettingsTransferEntity settings_transfer_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 2, "ConversationSettingsTransferEntity: row shape mismatch");
    return ConversationSettingsTransferEntity{row[0].as_text(), row[1].text_or_null()};
}

ConversationSettingsImportTransferEntity import_transfer_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 4, "ConversationSettingsImportTransferEntity: row shape mismatch");
    ConversationSettingsImportTransferEntity entity;
    entity.id = static_cast<int>(row[0].as_integer());
    entity.transfer_id = row[1].as_text();
    entity.settings_json = row[2].as_text();
    entity.mode = row[3].as_text();
    return entity;
}

EmbeddingEntity embedding_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 7, "EmbeddingEntity: row shape mismatch");
    EmbeddingEntity entity;
    entity.id = row[0].as_integer();
    entity.message_id = row[1].as_text();
    entity.model_id = row[2].as_text();
    entity.embedding = row[3].as_blob();
    entity.chunk_text = row[4].as_text();
    entity.dimension = static_cast<int>(row[5].as_integer());
    entity.source_fingerprint = row[6].text_or_null();
    return entity;
}

RunEntity run_from_row(const SqlRow& row) {
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
    run.end_reason = row[9].text_or_null().has_value()
                         ? std::optional<runtime::RunEndReason>(
                               run_end_reason_from_wire(*row[9].text_or_null()))
                         : std::nullopt;
    run.current_pass = static_cast<int>(row[10].as_integer());
    run.legacy_ambiguous = row[11].as_integer() != 0;
    run.validate();
    return run;
}

ExecutionMessageSummaryRow execution_row_from_row(const SqlRow& row) {
    kotlin_check(row.size() == 6, "ExecutionMessageSummaryRow: row shape mismatch");
    ExecutionMessageSummaryRow summary;
    summary.id = row[0].as_text();
    summary.conversation_id = row[1].as_text();
    summary.participant = participant_from_wire(row[2].as_text());
    summary.status = message_status_from_wire(row[3].as_text());
    summary.timestamp = row[4].as_integer();
    summary.preview = row[5].as_text();
    return summary;
}

std::optional<MessageEntity> one_message(SqlExecutor& sql, const std::string& query,
                                         const SqlArgs& args) {
    const std::vector<SqlRow> rows = sql.query(query, args);
    if (rows.empty()) return std::nullopt;
    return message_from_row(rows.front());
}

std::vector<MessageEntity> messages_from_rows(const std::vector<SqlRow>& rows) {
    std::vector<MessageEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(message_from_row(row));
    return out;
}

// ---- Kotlin semantics ----------------------------------------------------------------------

bool is_blank(std::string_view value) { return json::is_blank(value); }

bool message_equals_ignoring_sequence(const MessageEntity& left, const MessageEntity& right) {
    return left.id == right.id && left.conversation_id == right.conversation_id &&
           left.parent_id == right.parent_id && left.text == right.text &&
           left.images == right.images && left.thoughts == right.thoughts &&
           left.thought_title == right.thought_title && left.token_count == right.token_count &&
           left.input_token_count == right.input_token_count &&
           left.cached_input_token_count == right.cached_input_token_count &&
           left.cache_write_input_token_count == right.cache_write_input_token_count &&
           left.uncached_input_token_count == right.uncached_input_token_count &&
           left.output_token_count == right.output_token_count &&
           left.reasoning_token_count == right.reasoning_token_count &&
           left.generation_duration_ms == right.generation_duration_ms &&
           left.status == right.status && left.participant == right.participant &&
           left.timestamp == right.timestamp && left.thought_time_ms == right.thought_time_ms &&
           left.model_name == right.model_name && left.tool_call_json == right.tool_call_json &&
           left.attachment_meta == right.attachment_meta && left.run_id == right.run_id &&
           left.consumed_at_pass == right.consumed_at_pass &&
           left.source_json == right.source_json;
}

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

/**
 * ToolRoundCommitPolicy.requireValidShape, transliterated. The MessageSegment list is decoded by
 * the already-ported backup codec, which accepts the same JSON shape the app persists.
 */
std::string require_valid_tool_round(const std::vector<MessageEntity>& messages) {
    kotlin_require(messages.size() >= 2,
                   "A tool round requires a tool row and at least one result");
    {
        std::set<std::string> ids;
        for (const MessageEntity& message : messages) ids.insert(message.id);
        kotlin_require(ids.size() == messages.size(),
                       "A tool round cannot contain duplicate message ids");
    }
    const MessageEntity& tool_message = messages.front();
    const std::string& run_id = tool_message.run_id;
    kotlin_require(!run_id.empty(), "A tool round requires a durable Run");
    for (const MessageEntity& message : messages) {
        kotlin_require(message.run_id == run_id, "One tool round cannot span Runs");
    }
    for (const MessageEntity& message : messages) {
        kotlin_require(message.conversation_id == tool_message.conversation_id,
                       "One tool round cannot span conversations");
    }
    kotlin_require(tool_message.participant == recovery::Participant::model,
                   "A tool round must start with an assistant tool-call row");
    kotlin_require(tool_message.tool_call_json.has_value() &&
                       !is_blank(*tool_message.tool_call_json),
                   "A tool-call row requires protocol metadata");
    for (std::size_t i = 1; i < messages.size(); ++i) {
        const MessageEntity& result = messages[i];
        kotlin_require(result.participant == recovery::Participant::user &&
                           result.parent_id == tool_message.id &&
                           result.tool_call_json.has_value() &&
                           !is_blank(*result.tool_call_json),
                       "Every tool result must be complete and attached to the tool-call row");
    }

    auto decode = [](const MessageEntity& message) {
        const std::optional<std::vector<backup::MessageSegment>> segments =
            backup::decode_message_segments(*message.tool_call_json);
        if (!segments.has_value()) {
            throw KotlinError("IllegalArgumentException",
                              "Tool protocol metadata is not a valid segment list");
        }
        return *segments;
    };

    std::vector<std::string> request_ids;
    for (const backup::MessageSegment& segment : decode(tool_message)) {
        if (segment.type != "tool") continue;
        if (!segment.tool_call_id.has_value() || is_blank(*segment.tool_call_id)) {
            throw KotlinError("IllegalArgumentException", "Every tool request requires an id");
        }
        request_ids.push_back(*segment.tool_call_id);
    }
    kotlin_require(!request_ids.empty(), "A tool-call row requires at least one call");
    {
        std::set<std::string> ids(request_ids.begin(), request_ids.end());
        kotlin_require(ids.size() == request_ids.size(),
                       "A tool round cannot contain duplicate call ids");
    }

    std::vector<std::string> result_ids;
    for (std::size_t i = 1; i < messages.size(); ++i) {
        std::vector<backup::MessageSegment> tools;
        for (const backup::MessageSegment& segment : decode(messages[i])) {
            if (segment.type == "tool") tools.push_back(segment);
        }
        if (tools.size() != 1) {
            throw KotlinError("IllegalArgumentException",
                              "Each result row must contain exactly one tool result");
        }
        kotlin_require(tools.front().tool_result.has_value(),
                       "A tool result payload cannot be missing");
        if (!tools.front().tool_call_id.has_value() || is_blank(*tools.front().tool_call_id)) {
            throw KotlinError("IllegalArgumentException", "Every tool result requires a call id");
        }
        result_ids.push_back(*tools.front().tool_call_id);
    }
    kotlin_require(result_ids == request_ids,
                   "Tool request and result ids must form one complete ordered batch");
    return run_id;
}

/** ToolRoundCommitPolicy.resolveExactReplay; nullopt means no row exists yet. */
std::optional<std::vector<MessageEntity>> resolve_exact_replay(
    const std::vector<MessageEntity>& proposed,
    const std::vector<MessageEntity>& existing) {
    if (existing.empty()) return std::nullopt;
    kotlin_check(existing.size() == proposed.size(),
                 "A partially persisted tool round cannot be replayed");
    std::map<std::string, MessageEntity> by_id;
    for (const MessageEntity& message : existing) by_id[message.id] = message;
    std::vector<MessageEntity> ordered;
    ordered.reserve(proposed.size());
    for (const MessageEntity& message : proposed) {
        const auto found = by_id.find(message.id);
        kotlin_check(found != by_id.end(),
                     "A conflicting message id exists for the tool round");
        ordered.push_back(found->second);
    }
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        kotlin_check(message_equals_ignoring_sequence(ordered[i], proposed[i]),
                     "A conflicting tool round replay was rejected");
    }
    return ordered;
}

/** ChatEntity.copy(modelId = ..., lastUpdated = ...) */
ChatEntity with_admission_fields(const ChatEntity& conversation,
                                 const std::optional<std::string>& model_id,
                                 std::int64_t last_updated) {
    ChatEntity copy = conversation;
    copy.model_id = model_id;
    copy.last_updated = last_updated;
    return copy;
}

class RecoveryAdapter final : public runtime_recovery::RecoveryStore {
public:
    explicit RecoveryAdapter(ChatDao& dao) : dao_(dao) {}

    void begin_transaction() override {}
    void set_transaction_successful() override {}
    void end_transaction() override {}

    std::optional<runtime_recovery::ConversationSnapshot> get_conversation(
        std::string_view conversation_id) override {
        const std::optional<compact::ConversationEntry> conversation =
            dao_.compact::ContextCompactDao::get_conversation(conversation_id);
        if (!conversation.has_value()) return std::nullopt;
        runtime_recovery::ConversationSnapshot snapshot;
        snapshot.id = conversation->id;
        snapshot.selected_run_branches_json = conversation->selected_run_branches_json;
        return snapshot;
    }

    std::optional<runtime_recovery::RecoveryRun> get_live_run(
        std::string_view conversation_id) override {
        const std::optional<RunEntity> run =
            dao_.compact::ContextCompactDao::get_live_run(conversation_id);
        if (!run.has_value()) return std::nullopt;
        runtime_recovery::RecoveryRun snapshot;
        snapshot.id = run->id;
        snapshot.status = run->status;
        snapshot.current_pass = run->current_pass;
        return snapshot;
    }

    std::vector<runtime_recovery::RecoveryMessage> get_messages_for_runs(
        const std::vector<std::string>& run_ids) override {
        std::vector<runtime_recovery::RecoveryMessage> out;
        for (const MessageEntity& message :
             dao_.compact::ContextCompactDao::get_messages_for_runs(run_ids)) {
            runtime_recovery::RecoveryMessage recovered;
            recovered.id = message.id;
            recovered.participant = message.participant;
            recovered.status = message.status;
            recovered.tool_call_json = message.tool_call_json;
            recovered.text = message.text;
            recovered.images = message.images;
            recovered.thoughts = message.thoughts;
            recovered.thought_title = message.thought_title;
            recovered.token_count = message.token_count;
            recovered.input_token_count = message.input_token_count;
            recovered.cached_input_token_count = message.cached_input_token_count;
            recovered.cache_write_input_token_count = message.cache_write_input_token_count;
            recovered.uncached_input_token_count = message.uncached_input_token_count;
            recovered.output_token_count = message.output_token_count;
            recovered.reasoning_token_count = message.reasoning_token_count;
            recovered.generation_duration_ms = message.generation_duration_ms;
            recovered.thought_time_ms = message.thought_time_ms;
            out.push_back(std::move(recovered));
        }
        return out;
    }

    std::int64_t update_message_checkpoint(
        const runtime_recovery::MessageCheckpoint& checkpoint) override {
        MessageStreamCheckpoint mapped;
        mapped.id = checkpoint.id;
        mapped.text = checkpoint.text;
        mapped.images = checkpoint.images;
        mapped.thoughts = checkpoint.thoughts;
        mapped.thought_title = checkpoint.thought_title;
        mapped.token_count = checkpoint.token_count;
        mapped.input_token_count = checkpoint.input_token_count;
        mapped.cached_input_token_count = checkpoint.cached_input_token_count;
        mapped.cache_write_input_token_count = checkpoint.cache_write_input_token_count;
        mapped.uncached_input_token_count = checkpoint.uncached_input_token_count;
        mapped.output_token_count = checkpoint.output_token_count;
        mapped.reasoning_token_count = checkpoint.reasoning_token_count;
        mapped.generation_duration_ms = checkpoint.generation_duration_ms;
        mapped.status = checkpoint.status;
        mapped.thought_time_ms = checkpoint.thought_time_ms;
        mapped.tool_call_json = checkpoint.tool_call_json;
        return dao_.update_message_checkpoint(mapped);
    }

    std::int64_t terminalize_live_run(std::string_view run_id, runtime::RunStatus status,
                                      runtime::RunEndReason reason,
                                      std::int64_t at) override {
        return dao_.terminalize_live_run(run_id, status, reason, at);
    }

    std::int64_t compare_and_set_run_branch_selections(
        std::string_view conversation_id, std::string_view expected,
        std::string_view replacement) override {
        return dao_.compare_and_set_run_branch_selections(conversation_id, expected, replacement);
    }

    std::int64_t stop_stuck_messages_for_conversation(
        std::string_view conversation_id) override {
        return dao_.stop_stuck_messages_for_conversation(conversation_id);
    }

    std::int64_t touch_conversation_data(std::string_view conversation_id,
                                         std::int64_t at) override {
        return dao_.touch_conversation_data(conversation_id, at);
    }

    std::vector<selection::RunRef> get_runs_for_conversation_snapshot(
        std::string_view conversation_id) override {
        std::vector<selection::RunRef> out;
        for (const RunEntity& run : dao_.get_runs_for_conversation_snapshot(conversation_id)) {
            out.push_back(selection::RunRef{run.id, run.parent_run_id});
        }
        return out;
    }

private:
    ChatDao& dao_;
};

}  // namespace

void ConversationSettingsImportTransferEntity::validate() const {
    kotlin_require(id == 0);
    kotlin_require(!transfer_id.empty() && !is_blank(transfer_id));
    kotlin_require(mode == "MERGE" || mode == "REPLACE");
}

ChatDao::ChatDao(SqlExecutor& sql)
    : automation::ChatAutomationDao(sql),
      compact::ContextCompactDao(sql),
      provider_context::ChatProviderContextDao(sql),
      chat_search::ChatSearchDao(sql),
      media_references::ChatMediaReferencesDao(sql),
      new_chat::NewChatPersistDao(sql),
      sql_(sql),
      stack_(sql) {}

// ---- reads / projections -------------------------------------------------------------------

std::vector<ChatConversation> ChatDao::get_all_conversations() {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT id, title, systemPromptId, modelId, taskId, origin, graduated, "
        "hasUnreadGeneration, isPinned, selectedBranchesJson FROM conversations "
        "WHERE taskId IS NULL ORDER BY lastUpdated DESC",
        {});
    std::vector<ChatConversation> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(chat_conversation_from_row(row));
    return out;
}

std::vector<DrawerConversationRow> ChatDao::observe_drawer_conversations(int limit) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT id, title, hasUnreadGeneration, isPinned FROM conversations "
        "WHERE taskId IS NULL ORDER BY isPinned DESC, lastUpdated DESC, id LIMIT ?",
        {SqlValue::integer_value(limit)});
    std::vector<DrawerConversationRow> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(drawer_from_row(row));
    return out;
}

std::vector<DrawerConversationRow> ChatDao::get_drawer_conversations(
    const std::vector<std::string>& ids) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT id, title, hasUnreadGeneration, isPinned FROM conversations "
        "WHERE taskId IS NULL AND id IN (" +
            placeholders(ids.size()) + ")",
        text_args(ids));
    std::vector<DrawerConversationRow> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(drawer_from_row(row));
    return out;
}

std::vector<ChatEntity> ChatDao::get_executions_for_task(std::string_view task_id) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM conversations WHERE taskId = ? ORDER BY lastUpdated DESC",
        {SqlValue::text_value(std::string(task_id))});
    std::vector<ChatEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(chat_from_row(row));
    return out;
}

std::optional<ChatEntity> ChatDao::observe_conversation(std::string_view conversation_id) {
    const std::vector<SqlRow> rows = sql_.query("SELECT * FROM conversations WHERE id = ?",
                                                {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    return chat_from_row(rows.front());
}

std::optional<ConversationSettingsTransferEntity>
ChatDao::get_conversation_settings_transfer(std::string_view conversation_id) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM conversation_settings_transfer WHERE conversationId = ?",
        {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    return settings_transfer_from_row(rows.front());
}

std::vector<ConversationSettingsTransferEntity>
ChatDao::get_pending_conversation_settings_transfers() {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM conversation_settings_transfer ORDER BY conversationId", {});
    std::vector<ConversationSettingsTransferEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(settings_transfer_from_row(row));
    return out;
}

void ChatDao::upsert_conversation_settings_transfer(
    const ConversationSettingsTransferEntity& entity) {
    try {
        sql_.execute(
            "INSERT INTO conversation_settings_transfer (conversationId, settingsJson) "
            "VALUES (?, ?)",
            {SqlValue::text_value(entity.conversation_id), optional_text(entity.settings_json)});
    } catch (const SqlConstraintError&) {
        sql_.execute(
            "UPDATE conversation_settings_transfer SET settingsJson = ? WHERE conversationId = ?",
            {optional_text(entity.settings_json), SqlValue::text_value(entity.conversation_id)});
    }
}

int ChatDao::delete_conversation_settings_transfer(std::string_view conversation_id) {
    return static_cast<int>(sql_.execute(
        "DELETE FROM conversation_settings_transfer WHERE conversationId = ?",
        {SqlValue::text_value(std::string(conversation_id))}));
}

std::optional<ConversationSettingsImportTransferEntity>
ChatDao::get_conversation_settings_import_transfer() {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM conversation_settings_import_transfer WHERE id = 0", {});
    if (rows.empty()) return std::nullopt;
    return import_transfer_from_row(rows.front());
}

void ChatDao::upsert_conversation_settings_import_transfer(
    const ConversationSettingsImportTransferEntity& entity) {
    entity.validate();
    try {
        sql_.execute(
            "INSERT INTO conversation_settings_import_transfer (id, transferId, settingsJson, "
            "mode) VALUES (?, ?, ?, ?)",
            {SqlValue::integer_value(entity.id), SqlValue::text_value(entity.transfer_id),
             SqlValue::text_value(entity.settings_json), SqlValue::text_value(entity.mode)});
    } catch (const SqlConstraintError&) {
        sql_.execute(
            "UPDATE conversation_settings_import_transfer SET transferId = ?, settingsJson = ?, "
            "mode = ? WHERE id = ?",
            {SqlValue::text_value(entity.transfer_id), SqlValue::text_value(entity.settings_json),
             SqlValue::text_value(entity.mode), SqlValue::integer_value(entity.id)});
    }
}

int ChatDao::delete_conversation_settings_import_transfer(std::string_view transfer_id) {
    return static_cast<int>(sql_.execute(
        "DELETE FROM conversation_settings_import_transfer WHERE id = 0 AND transferId = ?",
        {SqlValue::text_value(std::string(transfer_id))}));
}

std::optional<MessageEntity> ChatDao::observe_message(std::string_view message_id) {
    return one_message(sql_, "SELECT * FROM messages WHERE id = ?",
                       {SqlValue::text_value(std::string(message_id))});
}

int ChatDao::stop_stuck_messages_for_conversation(std::string_view conversation_id) {
    return static_cast<int>(sql_.execute(
        "UPDATE messages SET status = 'STOPPED' WHERE conversationId = ? AND status IN "
        "('SENDING', 'THINKING', 'TOOL_CALLING', 'TRANSCRIBING')",
        {SqlValue::text_value(std::string(conversation_id))}));
}

// ---- conversation writes -------------------------------------------------------------------

void ChatDao::upsert_conversation(const ChatEntity& conversation) {
    try {
        sql_.execute(
            "INSERT INTO conversations (id, title, lastUpdated, dataChangedAt, "
            "selectedBranchesJson, systemPromptId, modelId, taskId, origin, graduated, draftText, "
            "draftAttachments, selectedRunBranchesJson, hasUnreadGeneration, isPinned) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            {SqlValue::text_value(conversation.id), SqlValue::text_value(conversation.title),
             SqlValue::integer_value(conversation.last_updated),
             SqlValue::integer_value(conversation.data_changed_at),
             optional_text(conversation.selected_branches_json),
             optional_text(conversation.system_prompt_id), optional_text(conversation.model_id),
             optional_text(conversation.task_id), SqlValue::text_value(conversation.origin),
             SqlValue::bool_value(conversation.graduated),
             SqlValue::text_value(conversation.draft_text),
             optional_text(conversation.draft_attachments),
             optional_text(conversation.selected_run_branches_json),
             SqlValue::bool_value(conversation.has_unread_generation),
             SqlValue::bool_value(conversation.is_pinned)});
    } catch (const SqlConstraintError&) {
        sql_.execute(
            "UPDATE conversations SET title = ?, lastUpdated = ?, dataChangedAt = ?, "
            "selectedBranchesJson = ?, systemPromptId = ?, modelId = ?, taskId = ?, origin = ?, "
            "graduated = ?, draftText = ?, draftAttachments = ?, selectedRunBranchesJson = ?, "
            "hasUnreadGeneration = ?, isPinned = ? WHERE id = ?",
            {SqlValue::text_value(conversation.title),
             SqlValue::integer_value(conversation.last_updated),
             SqlValue::integer_value(conversation.data_changed_at),
             optional_text(conversation.selected_branches_json),
             optional_text(conversation.system_prompt_id), optional_text(conversation.model_id),
             optional_text(conversation.task_id), SqlValue::text_value(conversation.origin),
             SqlValue::bool_value(conversation.graduated),
             SqlValue::text_value(conversation.draft_text),
             optional_text(conversation.draft_attachments),
             optional_text(conversation.selected_run_branches_json),
             SqlValue::bool_value(conversation.has_unread_generation),
             SqlValue::bool_value(conversation.is_pinned),
             SqlValue::text_value(conversation.id)});
    }
}

int ChatDao::update_conversation_model(std::string_view conversation_id,
                                       const std::optional<std::string>& model_id,
                                       std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET modelId = ?, dataChangedAt = MAX(dataChangedAt + 1, ?) "
        "WHERE id = ?",
        {optional_text(model_id), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(conversation_id))}));
}

int ChatDao::update_conversation_system_prompt(
    std::string_view conversation_id, const std::optional<std::string>& prompt_id,
    std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET systemPromptId = ?, dataChangedAt = MAX(dataChangedAt + 1, ?) "
        "WHERE id = ?",
        {optional_text(prompt_id), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(conversation_id))}));
}

int ChatDao::touch_conversation_data(std::string_view conversation_id, std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET dataChangedAt = MAX(dataChangedAt + 1, ?) WHERE id = ?",
        {SqlValue::integer_value(at), SqlValue::text_value(std::string(conversation_id))}));
}

void ChatDao::upsert_loop(const LoopEntity& loop, std::int64_t now_ms) {
    in_transaction([&] {
        automation::ChatAutomationDao::upsert_loop_row(loop);
        touch_conversation_data(loop.conversation_id, now_ms);
        return 0;
    });
}

int ChatDao::deactivate_loop_if_unchanged(std::string_view conversation_id,
                                          std::int64_t expected_revision,
                                          int expected_cycle_count,
                                          std::int64_t expected_interval_ms,
                                          std::int64_t expected_next_fire_at,
                                          int normalized_max_cycles, std::int64_t now_ms) {
    return in_transaction([&] {
        const int changed = automation::ChatAutomationDao::deactivate_loop_row_if_unchanged(
            conversation_id, expected_revision, expected_cycle_count, expected_interval_ms,
            expected_next_fire_at, normalized_max_cycles);
        if (changed > 0) touch_conversation_data(conversation_id, now_ms);
        return changed;
    });
}

void ChatDao::delete_loop(std::string_view conversation_id, std::int64_t now_ms) {
    in_transaction([&] {
        if (automation::ChatAutomationDao::delete_loop_row(conversation_id) > 0) {
            touch_conversation_data(conversation_id, now_ms);
        }
        return 0;
    });
}

int ChatDao::update_conversation_title(std::string_view conversation_id, std::string_view title,
                                       std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET title = ?, dataChangedAt = ? WHERE id = ?",
        {SqlValue::text_value(std::string(title)), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(conversation_id))}));
}

int ChatDao::set_conversation_pinned(std::string_view conversation_id, bool pinned,
                                     std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET isPinned = ?, dataChangedAt = MAX(dataChangedAt + 1, ?) "
        "WHERE id = ? AND taskId IS NULL AND isPinned != ?",
        {SqlValue::bool_value(pinned), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(conversation_id)), SqlValue::bool_value(pinned)}));
}

int ChatDao::set_conversation_unread_generation(std::string_view conversation_id, bool unread) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET hasUnreadGeneration = ? WHERE id = ? AND "
        "hasUnreadGeneration != ?",
        {SqlValue::bool_value(unread), SqlValue::text_value(std::string(conversation_id)),
         SqlValue::bool_value(unread)}));
}

int ChatDao::replace_conversation_model_references(
    std::string_view old_model_id, const std::optional<std::string>& new_model_id,
    std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET modelId = ?, dataChangedAt = MAX(dataChangedAt + 1, ?) "
        "WHERE modelId = ?",
        {optional_text(new_model_id), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(old_model_id))}));
}

int ChatDao::replace_new_chat_model_reference(
    std::string_view old_model_id, const std::optional<std::string>& new_model_id) {
    return static_cast<int>(sql_.execute(
        "UPDATE new_chat_persist SET modelId = ? WHERE id = 0 AND modelId = ?",
        {optional_text(new_model_id), SqlValue::text_value(std::string(old_model_id))}));
}

int ChatDao::update_conversation_title_if_unchanged(std::string_view conversation_id,
                                                    std::string_view expected_title,
                                                    std::string_view new_title,
                                                    std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET title = ?, dataChangedAt = ? WHERE id = ? AND title = ?",
        {SqlValue::text_value(std::string(new_title)), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(conversation_id)),
         SqlValue::text_value(std::string(expected_title))}));
}

// ---- message / run writes ------------------------------------------------------------------

void ChatDao::upsert_message(const MessageEntity& message) {
    try {
        compact::ContextCompactDao::insert_message(message);
    } catch (const SqlConstraintError&) {
        sql_.execute(
            "UPDATE messages SET conversationId = ?, parentId = ?, text = ?, images = ?, "
            "thoughts = ?, thoughtTitle = ?, tokenCount = ?, inputTokenCount = ?, "
            "cachedInputTokenCount = ?, cacheWriteInputTokenCount = ?, "
            "uncachedInputTokenCount = ?, outputTokenCount = ?, reasoningTokenCount = ?, "
            "generationDurationMs = ?, status = ?, participant = ?, timestamp = ?, "
            "thoughtTimeMs = ?, modelName = ?, toolCallJson = ?, attachmentMeta = ?, runId = ?, "
            "runSequence = ?, consumedAtPass = ?, sourceJson = ? WHERE id = ?",
            {SqlValue::text_value(message.conversation_id), optional_text(message.parent_id),
             SqlValue::text_value(message.text),
             SqlValue::text_value(encode_string_list(message.images)),
             optional_text(message.thoughts), optional_text(message.thought_title),
             SqlValue::integer_value(message.token_count),
             optional_int(message.input_token_count),
             optional_int(message.cached_input_token_count),
             optional_int(message.cache_write_input_token_count),
             optional_int(message.uncached_input_token_count),
             optional_int(message.output_token_count),
             optional_int(message.reasoning_token_count),
             optional_long(message.generation_duration_ms),
             SqlValue::text_value(message_status_to_wire(message.status)),
             SqlValue::text_value(participant_to_wire(message.participant)),
             SqlValue::integer_value(message.timestamp),
             optional_long(message.thought_time_ms), optional_text(message.model_name),
             optional_text(message.tool_call_json), optional_text(message.attachment_meta),
             SqlValue::text_value(message.run_id), SqlValue::integer_value(message.run_sequence),
             optional_int(message.consumed_at_pass), optional_text(message.source_json),
             SqlValue::text_value(message.id)});
    }
}

void ChatDao::upsert_run(const RunEntity& run) {
    run.validate();
    try {
        compact::ContextCompactDao::insert_run(run);
    } catch (const SqlConstraintError&) {
        sql_.execute(
            "UPDATE runs SET conversationId = ?, parentRunId = ?, status = ?, activeSlot = ?, "
            "startedAt = ?, lastCheckpointAt = ?, stopRequestedAt = ?, endedAt = ?, endReason = ?, "
            "currentPass = ?, legacyAmbiguous = ? WHERE id = ?",
            {SqlValue::text_value(run.conversation_id), optional_text(run.parent_run_id),
             SqlValue::text_value(run_status_to_wire(run.status)),
             optional_long(run.active_slot), SqlValue::integer_value(run.started_at),
             SqlValue::integer_value(run.last_checkpoint_at),
             optional_long(run.stop_requested_at), optional_long(run.ended_at),
             run.end_reason.has_value() ? SqlValue::text_value(run_end_reason_to_wire(*run.end_reason))
                                        : SqlValue::null(),
             SqlValue::integer_value(run.current_pass),
             SqlValue::bool_value(run.legacy_ambiguous), SqlValue::text_value(run.id)});
    }
}

std::vector<RunEntity> ChatDao::get_runs_for_conversation(std::string_view conversation_id) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM runs WHERE conversationId = ? ORDER BY startedAt, id",
        {SqlValue::text_value(std::string(conversation_id))});
    std::vector<RunEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(run_from_row(row));
    return out;
}

std::vector<RunEntity> ChatDao::get_runs_for_conversation_snapshot(
    std::string_view conversation_id) {
    return get_runs_for_conversation(conversation_id);
}

int ChatDao::update_conversation_for_run_admission(
    std::string_view conversation_id, std::string_view selected_branches_json,
    std::string_view selected_run_branches_json, const std::optional<std::string>& model_id,
    std::int64_t at, bool touch_conversation_on_admission) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET selectedBranchesJson = ?, selectedRunBranchesJson = ?, "
        "modelId = CASE WHEN ? IS NULL THEN modelId ELSE ? END, dataChangedAt = ?, "
        "lastUpdated = CASE WHEN ? THEN ? ELSE lastUpdated END WHERE id = ?",
        {SqlValue::text_value(std::string(selected_branches_json)),
         SqlValue::text_value(std::string(selected_run_branches_json)),
         optional_text(model_id), optional_text(model_id), SqlValue::integer_value(at),
         SqlValue::bool_value(touch_conversation_on_admission), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(conversation_id))}));
}

bool ChatDao::delete_message_subtree(std::string_view conversation_id,
                                     std::string_view root_message_id,
                                     const std::vector<std::string>& stale_message_ids,
                                     const std::vector<std::string>& root_run_ids_to_delete,
                                     std::string_view selected_branches_json,
                                     std::string_view selected_run_branches_json,
                                     std::int64_t at) {
    return in_transaction([&] {
        const std::optional<MessageEntity> root =
            compact::ContextCompactDao::get_message(root_message_id);
        if (!root.has_value()) return false;
        kotlin_require(root->conversation_id == conversation_id,
                       "Message " + std::string(root_message_id) +
                           " does not belong to conversation " + std::string(conversation_id));
        kotlin_require(contains(stale_message_ids, root_message_id));
        if (!stale_message_ids.empty()) {
            compact::ContextCompactDao::delete_embeddings_by_message_ids(stale_message_ids);
        }
        kotlin_check(
            compact::ContextCompactDao::update_selections_for_run_deletion(
                conversation_id, selected_branches_json, selected_run_branches_json, at) == 1,
            "Conversation " + std::string(conversation_id) +
                " disappeared during branch deletion");
        for (const std::string& run_id : root_run_ids_to_delete) {
            const std::optional<RunEntity> run =
                compact::ContextCompactDao::get_run(run_id);
            if (!run.has_value()) continue;
            kotlin_require(run->conversation_id == conversation_id,
                           "Run " + run_id + " does not belong to conversation " +
                               std::string(conversation_id));
            kotlin_check(compact::ContextCompactDao::delete_run(run_id) == 1,
                         "Run " + run_id + " disappeared during deletion");
        }
        compact::ContextCompactDao::delete_messages_by_ids(stale_message_ids);
        return true;
    });
}

RunGraphCommit ChatDao::create_run_with_messages_body(
    const RunEntity& run, const std::vector<MessageEntity>& messages,
    const SelectionMap& message_selection_updates,
    const std::optional<std::string>& conversation_model_id, std::int64_t at,
    bool touch_conversation_on_admission) {
    kotlin_require(run.status == runtime::RunStatus::Active);
    kotlin_require(run.active_slot == 1);
    kotlin_require(!messages.empty());
    kotlin_require(!conversation_model_id.has_value() || !is_blank(*conversation_model_id));
    for (const MessageEntity& message : messages) {
        kotlin_require(message.run_id == run.id);
    }
    for (std::size_t i = 0; i < messages.size(); ++i) {
        kotlin_require(messages[i].run_sequence == static_cast<std::int64_t>(i));
    }
    const std::optional<compact::ConversationEntry> conversation =
        compact::ContextCompactDao::get_conversation(run.conversation_id);
    kotlin_check(conversation.has_value(),
                 "Conversation " + run.conversation_id + " does not exist");
    kotlin_check(!compact::ContextCompactDao::get_live_run(run.conversation_id).has_value(),
                 "Conversation " + run.conversation_id + " already has a live Run");
    const std::set<std::string> inserted_message_ids = [&] {
        std::set<std::string> ids;
        for (const MessageEntity& message : messages) ids.insert(message.id);
        return ids;
    }();
    for (const auto& [key, value] : message_selection_updates) {
        (void)key;
        kotlin_require(inserted_message_ids.count(value) != 0,
                       "A new Run may only select messages committed in the same transaction");
    }

    compact::ContextCompactDao::insert_run(run);
    for (const MessageEntity& message : messages) {
        compact::ContextCompactDao::insert_message(message);
    }

    SelectionMap message_selections =
        compact::decode_compact_selections(conversation->selected_branches_json);
    for (const auto& [key, value] : message_selection_updates) {
        compact::selection_put(message_selections, key, value);
    }
    SelectionMap run_selections =
        compact::decode_compact_selections(conversation->selected_run_branches_json);
    compact::selection_put(run_selections, run.parent_run_id, run.id);
    kotlin_check(
        update_conversation_for_run_admission(
            run.conversation_id, compact::encode_compact_selections(message_selections),
            compact::encode_compact_selections(run_selections), conversation_model_id, at,
            touch_conversation_on_admission) == 1,
        "Conversation " + run.conversation_id + " disappeared during Run creation");
    return RunGraphCommit{messages, std::move(message_selections), std::move(run_selections)};
}

RunGraphCommit ChatDao::create_run_with_messages(
    const RunEntity& run, const std::vector<MessageEntity>& messages,
    const SelectionMap& message_selection_updates,
    const std::optional<std::string>& conversation_model_id, std::int64_t at,
    bool touch_conversation_on_admission) {
    run.validate();
    return in_transaction([&] {
        return create_run_with_messages_body(run, messages, message_selection_updates,
                                             conversation_model_id, at,
                                             touch_conversation_on_admission);
    });
}

RunGraphCommit ChatDao::create_conversation_run_with_messages(
    const ChatEntity& conversation, const RunEntity& run,
    const std::vector<MessageEntity>& messages, const SelectionMap& message_selection_updates,
    std::string_view conversation_model_id,
    const std::optional<std::string>& conversation_settings_json,
    const std::optional<NewChatPersistEntity>& expected_new_chat_persist, std::int64_t at) {
    run.validate();
    if (expected_new_chat_persist.has_value()) expected_new_chat_persist->validate();
    return in_transaction([&] {
        kotlin_require(conversation.id == run.conversation_id);
        kotlin_check(!compact::ContextCompactDao::get_conversation(conversation.id).has_value(),
                     "Conversation " + conversation.id + " already exists");
        kotlin_require(!is_blank(conversation_model_id));
        if (expected_new_chat_persist.has_value()) {
            const NewChatPersistEntity& expected = *expected_new_chat_persist;
            new_chat::NewChatPersistDao::delete_new_chat_persist_if_matches(
                expected.model_id, expected.system_prompt_id,
                expected.conversation_settings_json, expected.draft_text,
                expected.draft_attachments);
        }
        upsert_conversation(
            with_admission_fields(conversation, std::string(conversation_model_id), at));
        upsert_conversation_settings_transfer(
            ConversationSettingsTransferEntity{conversation.id, conversation_settings_json});
        return create_run_with_messages_body(
            run, messages, message_selection_updates, std::string(conversation_model_id), at,
            true);
    });
}

void ChatDao::import_run_graph_body(const std::vector<RunEntity>& runs,
                                    const std::vector<MessageEntity>& messages) {
    for (const RunEntity& run : runs) {
        run.validate();
        kotlin_require(runtime::is_terminal(run.status), "Imported Runs must be terminal");
    }
    std::set<std::string> incoming_run_ids;
    for (const RunEntity& run : runs) incoming_run_ids.insert(run.id);
    for (const MessageEntity& message : messages) {
        kotlin_require(incoming_run_ids.count(message.run_id) != 0 ||
                           compact::ContextCompactDao::get_run(message.run_id).has_value(),
                       "Every imported message must reference an imported or existing Run");
    }
    for (const RunEntity& run : runs) {
        if (!compact::ContextCompactDao::get_run(run.id).has_value()) {
            compact::ContextCompactDao::insert_run(run);
        }
    }
    for (const MessageEntity& message : messages) upsert_message(message);
}

void ChatDao::import_run_graph(const std::vector<RunEntity>& runs,
                               const std::vector<MessageEntity>& messages) {
    in_transaction([&] {
        import_run_graph_body(runs, messages);
        return 0;
    });
}

void ChatDao::replace_imported_conversation_graph(
    const std::vector<ChatEntity>& conversations, const std::vector<RunEntity>& runs,
    const std::vector<MessageEntity>& messages) {
    in_transaction([&] {
        std::set<std::string> conversation_ids;
        for (const ChatEntity& conversation : conversations) {
            conversation_ids.insert(conversation.id);
        }
        kotlin_require(conversation_ids.size() == conversations.size(),
                       "Imported conversation IDs must be unique");
        std::set<std::string> run_ids;
        for (const RunEntity& run : runs) run_ids.insert(run.id);
        kotlin_require(run_ids.size() == runs.size(), "Imported Run IDs must be unique");
        std::set<std::string> message_ids;
        for (const MessageEntity& message : messages) message_ids.insert(message.id);
        kotlin_require(message_ids.size() == messages.size(),
                       "Imported message IDs must be unique");

        std::map<std::string, RunEntity> runs_by_id;
        for (const RunEntity& run : runs) runs_by_id[run.id] = run;
        std::map<std::string, MessageEntity> messages_by_id;
        for (const MessageEntity& message : messages) messages_by_id[message.id] = message;

        for (const RunEntity& run : runs) {
            run.validate();
            kotlin_require(runtime::is_terminal(run.status) && !run.active_slot.has_value() &&
                               conversation_ids.count(run.conversation_id) != 0,
                           "Every imported Run must be terminal and belong to an imported "
                           "conversation");
        }
        for (const RunEntity& run : runs) {
            if (!run.parent_run_id.has_value()) continue;
            const auto parent = runs_by_id.find(*run.parent_run_id);
            kotlin_require(parent != runs_by_id.end() &&
                               parent->second.conversation_id == run.conversation_id,
                           "Every imported parent Run must belong to the same replacement "
                           "conversation");
        }
        for (const MessageEntity& message : messages) {
            const auto run = runs_by_id.find(message.run_id);
            const bool parent_ok = !message.parent_id.has_value() ||
                                   (messages_by_id.count(*message.parent_id) != 0 &&
                                    messages_by_id[*message.parent_id].conversation_id ==
                                        message.conversation_id);
            kotlin_require(conversation_ids.count(message.conversation_id) != 0 &&
                               run != runs_by_id.end() &&
                               run->second.conversation_id == message.conversation_id &&
                               parent_ok,
                           "Every imported message and parent must belong to its replacement "
                           "conversation");
        }

        delete_all_conversations();
        for (const ChatEntity& conversation : conversations) upsert_conversation(conversation);
        for (const RunEntity& run : runs) compact::ContextCompactDao::insert_run(run);
        for (const MessageEntity& message : messages) upsert_message(message);
        return 0;
    });
}

void ChatDao::create_fork_graph(
    const ChatEntity& conversation, const std::vector<RunEntity>& runs,
    const std::vector<MessageEntity>& messages,
    const std::vector<std::pair<std::string, std::string>>& source_to_fork_message_ids) {
    in_transaction([&] {
        kotlin_require(!compact::ContextCompactDao::get_conversation(conversation.id).has_value(),
                       "Fork conversation " + conversation.id + " already exists");
        kotlin_require(!runs.empty());
        kotlin_require(!messages.empty());
        for (const RunEntity& run : runs) {
            kotlin_require(run.conversation_id == conversation.id);
        }
        for (const MessageEntity& message : messages) {
            kotlin_require(message.conversation_id == conversation.id);
        }
        std::set<std::string> run_ids;
        for (const RunEntity& run : runs) run_ids.insert(run.id);
        std::set<std::string> message_ids;
        for (const MessageEntity& message : messages) message_ids.insert(message.id);
        for (const RunEntity& run : runs) {
            kotlin_require(!run.parent_run_id.has_value() ||
                               run_ids.count(*run.parent_run_id) != 0,
                           "Every forked Run must reference another forked Run");
        }
        for (const MessageEntity& message : messages) {
            kotlin_require(!message.parent_id.has_value() ||
                               message_ids.count(*message.parent_id) != 0,
                           "Every forked message must reference another forked message");
        }
        for (const MessageEntity& message : messages) {
            kotlin_require(run_ids.count(message.run_id) != 0,
                           "Every forked message must reference a forked Run");
        }
        kotlin_require(source_to_fork_message_ids.size() == messages.size());
        std::set<std::string> fork_values;
        for (const auto& [source, fork] : source_to_fork_message_ids) {
            (void)source;
            fork_values.insert(fork);
        }
        kotlin_require(fork_values == message_ids,
                       "Every forked message must have exactly one source message");
        for (const auto& [source, fork] : source_to_fork_message_ids) {
            (void)fork;
            kotlin_require(message_ids.count(source) == 0,
                           "Forked messages must not reuse source message identities");
        }

        std::vector<std::string> source_ids;
        for (const auto& [source, fork] : source_to_fork_message_ids) {
            (void)fork;
            source_ids.push_back(source);
        }
        const std::vector<EmbeddingEntity> source_embeddings =
            get_embeddings_by_message_ids(source_ids);
        std::vector<EmbeddingEntity> cloned;
        cloned.reserve(source_embeddings.size());
        for (const EmbeddingEntity& source : source_embeddings) {
            std::optional<std::string> fork_message_id;
            for (const auto& [from, to] : source_to_fork_message_ids) {
                if (from == source.message_id) {
                    fork_message_id = to;
                    break;
                }
            }
            kotlin_require(fork_message_id.has_value(),
                           "Missing fork message mapping for embedding " +
                               std::to_string(source.id) + " (" + source.message_id + ")");
            EmbeddingEntity copy = source;
            copy.id = 0;
            copy.message_id = *fork_message_id;
            cloned.push_back(std::move(copy));
        }

        upsert_conversation(conversation);
        import_run_graph_body(runs, messages);
        if (!cloned.empty()) {
            const std::vector<std::int64_t> inserted_ids = insert_embeddings(cloned);
            bool positive = true;
            for (const std::int64_t id : inserted_ids) {
                if (id <= 0) positive = false;
            }
            kotlin_check(inserted_ids.size() == cloned.size() && positive,
                         "Every forked embedding must be inserted as a new database row");
        }
        return 0;
    });
}

ToolRoundCommit ChatDao::append_tool_round_to_run(const std::vector<MessageEntity>& messages,
                                                  int expected_pass,
                                                  std::int64_t now_ms) {
    return in_transaction([&] {
        kotlin_require(expected_pass >= 0);
        const std::string run_id = require_valid_tool_round(messages);
        std::vector<MessageEntity> existing;
        for (const MessageEntity& message : messages) {
            const std::optional<MessageEntity> row =
                compact::ContextCompactDao::get_message(message.id);
            if (row.has_value()) existing.push_back(*row);
        }
        if (const std::optional<std::vector<MessageEntity>> replay =
                resolve_exact_replay(messages, existing);
            replay.has_value()) {
            return ToolRoundCommit{*replay, false};
        }
        const std::optional<RunEntity> run =
            compact::ContextCompactDao::get_run(run_id);
        kotlin_check(run.has_value(), "Run " + run_id + " does not exist");
        kotlin_check(expected_pass >= 0 && run->id == run_id &&
                         run->status == runtime::RunStatus::Active &&
                         run->active_slot == 1 && run->current_pass == expected_pass,
                     "Cannot append tool round to non-current Run " + run_id + " Pass " +
                         std::to_string(expected_pass));
        const std::int64_t first_sequence =
            compact::ContextCompactDao::next_run_sequence(run_id);
        std::vector<MessageEntity> assigned;
        assigned.reserve(messages.size());
        std::int64_t max_timestamp = messages.front().timestamp;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            MessageEntity copy = messages[i];
            copy.run_sequence = first_sequence + static_cast<std::int64_t>(i);
            max_timestamp = std::max(max_timestamp, copy.timestamp);
            assigned.push_back(std::move(copy));
        }
        for (const MessageEntity& message : assigned) {
            compact::ContextCompactDao::insert_message(message);
        }
        compact::ContextCompactDao::touch_run(
            run_id, std::max(run->last_checkpoint_at, max_timestamp));
        kotlin_check(touch_conversation_data(run->conversation_id, now_ms) == 1);
        return ToolRoundCommit{std::move(assigned), true};
    });
}

int ChatDao::mark_run_stopping(std::string_view run_id, std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE runs SET status = 'STOPPING', stopRequestedAt = ?, lastCheckpointAt = ? "
        "WHERE id = ? AND status = 'ACTIVE' AND activeSlot = 1",
        {SqlValue::integer_value(at), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(run_id))}));
}

int ChatDao::terminalize_live_run(std::string_view run_id, runtime::RunStatus status,
                                  runtime::RunEndReason reason, std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE runs SET status = ?, activeSlot = NULL, lastCheckpointAt = ?, endedAt = ?, "
        "endReason = ? WHERE id = ? AND activeSlot = 1",
        {SqlValue::text_value(run_status_to_wire(status)), SqlValue::integer_value(at),
         SqlValue::integer_value(at), SqlValue::text_value(run_end_reason_to_wire(reason)),
         SqlValue::text_value(std::string(run_id))}));
}

int ChatDao::stop_in_flight_model_messages(std::string_view run_id) {
    return static_cast<int>(sql_.execute(
        "UPDATE messages SET status = 'STOPPED' WHERE runId = ? AND participant = 'MODEL' AND "
        "status IN ('SENDING', 'THINKING', 'TOOL_CALLING', 'TRANSCRIBING')",
        {SqlValue::text_value(std::string(run_id))}));
}

bool ChatDao::finish_generation(const MessageStreamCheckpoint& checkpoint,
                                std::string_view conversation_id, std::string_view run_id,
                                runtime::RunStatus status, runtime::RunEndReason reason,
                                std::int64_t at, bool mark_conversation_unread) {
    return in_transaction([&] {
        kotlin_require(runtime::is_terminal(status));
        const bool message_updated = update_message_checkpoint(checkpoint) == 1;
        const bool run_updated = terminalize_live_run(run_id, status, reason, at) == 1;
        const bool completed = message_updated && run_updated;
        if (completed) {
            kotlin_check(touch_conversation_data(conversation_id, at) == 1);
        }
        if (completed && mark_conversation_unread) {
            set_conversation_unread_generation(conversation_id, true);
        }
        return completed;
    });
}

bool ChatDao::finish_stopped_generation(
    const std::vector<MessageStreamCheckpoint>& checkpoints, std::string_view conversation_id,
    const std::optional<std::string>& run_id, std::int64_t at) {
    return in_transaction([&] {
        for (const MessageStreamCheckpoint& checkpoint : checkpoints) {
            update_message_checkpoint(checkpoint);
        }
        if (run_id.has_value()) stop_in_flight_model_messages(*run_id);
        const bool completed =
            !run_id.has_value() ||
            terminalize_live_run(*run_id, runtime::RunStatus::Stopped,
                                 runtime::RunEndReason::UserStopped, at) == 1;
        if (completed) kotlin_check(touch_conversation_data(conversation_id, at) == 1);
        return completed;
    });
}

int ChatDao::recover_conversation_runtime(std::string_view conversation_id, std::int64_t at) {
    return in_transaction([&] {
        RecoveryAdapter adapter(*this);
        return static_cast<int>(
            runtime_recovery::execute_runtime_recovery(adapter, conversation_id, at));
    });
}

bool ChatDao::update_conversation_message_checkpoint(
    std::string_view conversation_id, const MessageStreamCheckpoint& checkpoint,
    std::int64_t at) {
    return in_transaction([&] {
        const bool updated = update_message_checkpoint(checkpoint) == 1;
        if (updated) kotlin_check(touch_conversation_data(conversation_id, at) == 1);
        return updated;
    });
}

int ChatDao::update_message_checkpoint(const MessageStreamCheckpoint& checkpoint) {
    return static_cast<int>(sql_.execute(
        "UPDATE messages SET text = ?, images = ?, thoughts = ?, thoughtTitle = ?, "
        "tokenCount = ?, inputTokenCount = ?, cachedInputTokenCount = ?, "
        "cacheWriteInputTokenCount = ?, uncachedInputTokenCount = ?, outputTokenCount = ?, "
        "reasoningTokenCount = ?, generationDurationMs = ?, status = ?, thoughtTimeMs = ?, "
        "toolCallJson = ? WHERE id = ?",
        {SqlValue::text_value(checkpoint.text),
         SqlValue::text_value(encode_string_list(checkpoint.images)),
         optional_text(checkpoint.thoughts), optional_text(checkpoint.thought_title),
         SqlValue::integer_value(checkpoint.token_count),
         optional_int(checkpoint.input_token_count),
         optional_int(checkpoint.cached_input_token_count),
         optional_int(checkpoint.cache_write_input_token_count),
         optional_int(checkpoint.uncached_input_token_count),
         optional_int(checkpoint.output_token_count),
         optional_int(checkpoint.reasoning_token_count),
         optional_long(checkpoint.generation_duration_ms),
         SqlValue::text_value(message_status_to_wire(checkpoint.status)),
         optional_long(checkpoint.thought_time_ms), optional_text(checkpoint.tool_call_json),
         SqlValue::text_value(checkpoint.id)}));
}

// ---- deletes -------------------------------------------------------------------------------

void ChatDao::delete_conversation(std::string_view conversation_id) {
    sql_.execute("DELETE FROM conversations WHERE id = ?",
                 {SqlValue::text_value(std::string(conversation_id))});
}

void ChatDao::delete_messages_by_conversation(std::string_view conversation_id) {
    sql_.execute("DELETE FROM messages WHERE conversationId = ?",
                 {SqlValue::text_value(std::string(conversation_id))});
}

int ChatDao::update_run_parent(std::string_view run_id,
                               const std::optional<std::string>& new_parent_run_id) {
    return static_cast<int>(sql_.execute(
        "UPDATE runs SET parentRunId = ? WHERE id = ?",
        {optional_text(new_parent_run_id), SqlValue::text_value(std::string(run_id))}));
}

void ChatDao::delete_embeddings_by_conversation(std::string_view conversation_id) {
    sql_.execute(
        "DELETE FROM embeddings WHERE messageId IN (SELECT id FROM messages WHERE "
        "conversationId = ?)",
        {SqlValue::text_value(std::string(conversation_id))});
}

void ChatDao::delete_orphaned_embeddings() {
    sql_.execute(
        "DELETE FROM embeddings WHERE messageId LIKE 'compact_%' OR NOT EXISTS "
        "(SELECT 1 FROM messages WHERE messages.id = embeddings.messageId)",
        {});
}

std::optional<MessageEntity> ChatDao::get_last_message_for_conversation(
    std::string_view conversation_id) {
    return one_message(sql_,
                       "SELECT * FROM messages WHERE conversationId = ? ORDER BY timestamp DESC "
                       "LIMIT 1",
                       {SqlValue::text_value(std::string(conversation_id))});
}

std::vector<ExecutionMessageSummaryRow> ChatDao::observe_execution_messages_for_task(
    std::string_view task_id) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT m.id, m.conversationId, m.participant, m.status, m.timestamp, "
        "substr(m.text, 1, 500) AS preview FROM messages m INNER JOIN conversations c ON "
        "m.conversationId = c.id WHERE c.taskId = ? AND m.participant IN ('MODEL', 'ERROR') "
        "ORDER BY m.timestamp ASC",
        {SqlValue::text_value(std::string(task_id))});
    std::vector<ExecutionMessageSummaryRow> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(execution_row_from_row(row));
    return out;
}

// ---- embeddings ----------------------------------------------------------------------------

std::vector<std::int64_t> ChatDao::insert_embeddings(
    const std::vector<EmbeddingEntity>& embeddings) {
    std::vector<std::int64_t> ids;
    ids.reserve(embeddings.size());
    for (const EmbeddingEntity& entity : embeddings) {
        if (entity.id == 0) {
            sql_.execute(
                "INSERT INTO embeddings (messageId, modelId, embedding, chunkText, dimension, "
                "sourceFingerprint) VALUES (?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(entity.message_id), SqlValue::text_value(entity.model_id),
                 SqlValue{SqlValue::Kind::Blob, "", 0, 0.0, entity.embedding},
                 SqlValue::text_value(entity.chunk_text),
                 SqlValue::integer_value(entity.dimension),
                 optional_text(entity.source_fingerprint)});
        } else {
            sql_.execute(
                "INSERT INTO embeddings (id, messageId, modelId, embedding, chunkText, dimension, "
                "sourceFingerprint) VALUES (?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::integer_value(entity.id),
                 SqlValue::text_value(entity.message_id), SqlValue::text_value(entity.model_id),
                 SqlValue{SqlValue::Kind::Blob, "", 0, 0.0, entity.embedding},
                 SqlValue::text_value(entity.chunk_text),
                 SqlValue::integer_value(entity.dimension),
                 optional_text(entity.source_fingerprint)});
        }
        ids.push_back(sql_.last_insert_rowid());
    }
    return ids;
}

std::vector<EmbeddingEntity> ChatDao::get_embeddings_by_message_ids(
    const std::vector<std::string>& message_ids) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM embeddings WHERE messageId IN (" + placeholders(message_ids.size()) + ")",
        text_args(message_ids));
    std::vector<EmbeddingEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(embedding_from_row(row));
    return out;
}

std::optional<EmbeddingEntity> ChatDao::get_embedding(std::string_view message_id) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM embeddings WHERE messageId = ? LIMIT 1",
        {SqlValue::text_value(std::string(message_id))});
    if (rows.empty()) return std::nullopt;
    return embedding_from_row(rows.front());
}

std::vector<EmbeddingEntity> ChatDao::get_all_embeddings() {
    const std::vector<SqlRow> rows = sql_.query("SELECT * FROM embeddings", {});
    std::vector<EmbeddingEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(embedding_from_row(row));
    return out;
}

// ---- bulk export / import ------------------------------------------------------------------

std::vector<MessageEntity> ChatDao::get_messages_by_ids(const std::vector<std::string>& ids) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM messages WHERE id IN (" + placeholders(ids.size()) + ")", text_args(ids));
    return messages_from_rows(rows);
}

void ChatDao::update_draft(std::string_view id, std::string_view text,
                           const std::optional<std::string>& attachments, std::int64_t at) {
    sql_.execute(
        "UPDATE conversations SET draftText = ?, draftAttachments = ?, dataChangedAt = ? "
        "WHERE id = ?",
        {SqlValue::text_value(std::string(text)), optional_text(attachments),
         SqlValue::integer_value(at), SqlValue::text_value(std::string(id))});
}

std::vector<ChatEntity> ChatDao::get_all_conversations_list() {
    const std::vector<SqlRow> rows = sql_.query("SELECT * FROM conversations", {});
    std::vector<ChatEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(chat_from_row(row));
    return out;
}

int ChatDao::compare_and_set_run_branch_selections(std::string_view conversation_id,
                                                   std::string_view expected,
                                                   std::string_view replacement) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET selectedRunBranchesJson = ? WHERE id = ? AND "
        "selectedRunBranchesJson = ?",
        {SqlValue::text_value(std::string(replacement)),
         SqlValue::text_value(std::string(conversation_id)),
         SqlValue::text_value(std::string(expected))}));
}

std::vector<std::string> ChatDao::get_all_conversation_ids() {
    const std::vector<SqlRow> rows = sql_.query("SELECT id FROM conversations", {});
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(row[0].as_text());
    return out;
}

std::vector<std::string> ChatDao::get_all_task_ids() {
    const std::vector<SqlRow> rows = sql_.query("SELECT id FROM tasks", {});
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(row[0].as_text());
    return out;
}

std::vector<MessageEntity> ChatDao::get_messages_page(
    const std::optional<std::string>& after_id, int limit) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM messages WHERE (? IS NULL OR id > ?) ORDER BY id LIMIT ?",
        {optional_text(after_id), optional_text(after_id), SqlValue::integer_value(limit)});
    return messages_from_rows(rows);
}

std::vector<MessageEntity> ChatDao::get_conversation_messages_page(
    std::string_view conversation_id, const std::optional<std::string>& after_id, int limit) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT * FROM messages WHERE conversationId = ? AND (? IS NULL OR id > ?) ORDER BY id "
        "LIMIT ?",
        {SqlValue::text_value(std::string(conversation_id)), optional_text(after_id),
         optional_text(after_id), SqlValue::integer_value(limit)});
    return messages_from_rows(rows);
}

void ChatDao::delete_all_conversations() { sql_.execute("DELETE FROM conversations", {}); }

std::vector<std::string> ChatDao::find_existing_message_ids(
    const std::vector<std::string>& ids) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT id FROM messages WHERE id IN (" + placeholders(ids.size()) + ")", text_args(ids));
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(row[0].as_text());
    return out;
}

// ---- configured model/provider rewrites ----------------------------------------------------

void ChatDao::replace_configured_model_references(
    std::string_view old_model_id, const std::optional<std::string>& new_model_id,
    std::int64_t now_ms) {
    in_transaction([&] {
        replace_conversation_model_references(old_model_id, new_model_id, now_ms);
        replace_new_chat_model_reference(old_model_id, new_model_id);
        automation::ChatAutomationDao::replace_task_model_references(old_model_id, new_model_id);
        return 0;
    });
}

int ChatDao::rename_conversation_provider_model_references(std::string_view old_provider,
                                                           std::string_view new_provider,
                                                           std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET modelId = ? || substr(modelId, length(?) + 1), "
        "dataChangedAt = MAX(dataChangedAt + 1, ?) WHERE substr(modelId, 1, length(?) + 1) = "
        "? || ':'",
        {SqlValue::text_value(std::string(new_provider)),
         SqlValue::text_value(std::string(old_provider)), SqlValue::integer_value(at),
         SqlValue::text_value(std::string(old_provider)),
         SqlValue::text_value(std::string(old_provider))}));
}

int ChatDao::touch_conversations_with_message_provider(std::string_view old_provider,
                                                       std::int64_t at) {
    return static_cast<int>(sql_.execute(
        "UPDATE conversations SET dataChangedAt = MAX(dataChangedAt + 1, ?) WHERE id IN "
        "(SELECT conversationId FROM messages WHERE substr(modelName, 1, length(?) + 1) = "
        "? || ':')",
        {SqlValue::integer_value(at), SqlValue::text_value(std::string(old_provider)),
         SqlValue::text_value(std::string(old_provider))}));
}

int ChatDao::rename_new_chat_provider_model_reference(std::string_view old_provider,
                                                      std::string_view new_provider) {
    return static_cast<int>(sql_.execute(
        "UPDATE new_chat_persist SET modelId = ? || substr(modelId, length(?) + 1) WHERE id = 0 "
        "AND substr(modelId, 1, length(?) + 1) = ? || ':'",
        {SqlValue::text_value(std::string(new_provider)),
         SqlValue::text_value(std::string(old_provider)),
         SqlValue::text_value(std::string(old_provider)),
         SqlValue::text_value(std::string(old_provider))}));
}

int ChatDao::rename_message_provider_model_references(std::string_view old_provider,
                                                      std::string_view new_provider) {
    return static_cast<int>(sql_.execute(
        "UPDATE messages SET modelName = ? || substr(modelName, length(?) + 1) WHERE "
        "substr(modelName, 1, length(?) + 1) = ? || ':'",
        {SqlValue::text_value(std::string(new_provider)),
         SqlValue::text_value(std::string(old_provider)),
         SqlValue::text_value(std::string(old_provider)),
         SqlValue::text_value(std::string(old_provider))}));
}

void ChatDao::rename_configured_provider_model_references(std::string_view old_provider,
                                                          std::string_view new_provider,
                                                          std::int64_t now_ms) {
    in_transaction([&] {
        const std::int64_t at = now_ms;
        rename_conversation_provider_model_references(old_provider, new_provider, at);
        rename_new_chat_provider_model_reference(old_provider, new_provider);
        automation::ChatAutomationDao::rename_task_provider_model_references(old_provider,
                                                                            new_provider);
        touch_conversations_with_message_provider(old_provider, at);
        rename_message_provider_model_references(old_provider, new_provider);
        return 0;
    });
}

}  // namespace agora::core::chat_dao
