// Port of data/local/migration/Migration16To17.kt.
//
// The SQL text, statement order and bind-argument types are part of the room schema contract, so
// they are reproduced literally. Cursor plumbing is the platform boundary: the port asks the
// Database interface for the same projections the Kotlin migration reads.
#include "agora/core/migration_16_17.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/legacy_run_backfill.hpp"

namespace agora::core::migration_16_17 {
namespace {

using legacy_run_backfill::LegacyMessageRecord;
using legacy_run_backfill::NullableSelections;

constexpr char kAlterConversations[] =
    "ALTER TABLE conversations ADD COLUMN selectedRunBranchesJson TEXT";

constexpr char kCreateRuns[] = R"(CREATE TABLE IF NOT EXISTS runs (
    id TEXT PRIMARY KEY NOT NULL,
    conversationId TEXT NOT NULL,
    parentRunId TEXT,
    status TEXT NOT NULL,
    activeSlot INTEGER,
    startedAt INTEGER NOT NULL,
    lastCheckpointAt INTEGER NOT NULL,
    stopRequestedAt INTEGER,
    endedAt INTEGER,
    endReason TEXT,
    currentPass INTEGER NOT NULL,
    legacyAmbiguous INTEGER NOT NULL,
    FOREIGN KEY(conversationId) REFERENCES conversations(id) ON UPDATE NO ACTION ON DELETE CASCADE,
    FOREIGN KEY(parentRunId) REFERENCES runs(id) ON UPDATE NO ACTION ON DELETE CASCADE
))";

constexpr char kIndexRunsConversation[] =
    "CREATE INDEX IF NOT EXISTS index_runs_conversationId ON runs (conversationId)";
constexpr char kIndexRunsParent[] =
    "CREATE INDEX IF NOT EXISTS index_runs_parentRunId ON runs (parentRunId)";
constexpr char kIndexRunsActiveSlot[] =
    "CREATE UNIQUE INDEX IF NOT EXISTS index_runs_conversationId_activeSlot "
    "ON runs (conversationId, activeSlot)";

constexpr char kAlterMessagesRunId[] = "ALTER TABLE messages ADD COLUMN runId TEXT";
constexpr char kAlterMessagesSequence[] = "ALTER TABLE messages ADD COLUMN runSequence INTEGER";
constexpr char kAlterMessagesConsumed[] = "ALTER TABLE messages ADD COLUMN consumedAtPass INTEGER";

constexpr char kSelectConversations[] = "SELECT id, selectedBranchesJson FROM conversations";

constexpr char kSelectLegacyMessages[] = R"(SELECT id, parentId, participant, status, timestamp
FROM messages
WHERE conversationId = ?)";

constexpr char kInsertRun[] = R"(INSERT INTO runs (
    id, conversationId, parentRunId, status, activeSlot, startedAt,
    lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass,
    legacyAmbiguous
) VALUES (?, ?, ?, ?, NULL, ?, ?, NULL, ?, ?, 0, ?))";

constexpr char kUpdateMessageAssignment[] = R"(UPDATE messages
SET runId = ?, runSequence = ?, consumedAtPass = ?
WHERE id = ?)";

constexpr char kUpdateConversationSelections[] =
    "UPDATE conversations SET selectedRunBranchesJson = ? WHERE id = ?";

constexpr char kCountUnassigned[] =
    "SELECT COUNT(*) FROM messages WHERE runId IS NULL OR runSequence IS NULL";

constexpr char kCreateMessagesNew[] = R"(CREATE TABLE IF NOT EXISTS messages_new (
    id TEXT PRIMARY KEY NOT NULL,
    conversationId TEXT NOT NULL,
    parentId TEXT,
    text TEXT NOT NULL,
    images TEXT NOT NULL,
    thoughts TEXT,
    thoughtTitle TEXT,
    tokenCount INTEGER NOT NULL,
    status TEXT NOT NULL,
    participant TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    thoughtTimeMs INTEGER,
    modelName TEXT,
    toolCallJson TEXT,
    attachmentMeta TEXT,
    runId TEXT NOT NULL,
    runSequence INTEGER NOT NULL,
    consumedAtPass INTEGER,
    FOREIGN KEY(conversationId) REFERENCES conversations(id) ON UPDATE NO ACTION ON DELETE CASCADE,
    FOREIGN KEY(runId) REFERENCES runs(id) ON UPDATE NO ACTION ON DELETE CASCADE
))";

constexpr char kCopyMessages[] = R"(INSERT INTO messages_new (
    id, conversationId, parentId, text, images, thoughts, thoughtTitle, tokenCount,
    status, participant, timestamp, thoughtTimeMs, modelName, toolCallJson,
    attachmentMeta, runId, runSequence, consumedAtPass
)
SELECT
    id, conversationId, parentId, text, images, thoughts, thoughtTitle, tokenCount,
    status, participant, timestamp, thoughtTimeMs, modelName, toolCallJson,
    attachmentMeta, runId, runSequence, consumedAtPass
FROM messages)";

constexpr char kDropMessages[] = "DROP TABLE messages";
constexpr char kRenameMessages[] = "ALTER TABLE messages_new RENAME TO messages";
constexpr char kIndexMessagesConversation[] =
    "CREATE INDEX IF NOT EXISTS index_messages_conversationId ON messages (conversationId)";
constexpr char kIndexMessagesRun[] =
    "CREATE INDEX IF NOT EXISTS index_messages_runId ON messages (runId)";

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

legacy_run_backfill::Participant parse_participant(const std::string& name) {
    if (name == "USER") return legacy_run_backfill::Participant::User;
    if (name == "MODEL") return legacy_run_backfill::Participant::Model;
    if (name == "ERROR") return legacy_run_backfill::Participant::Error;
    return legacy_run_backfill::Participant::Error;
}

legacy_run_backfill::MessageStatus parse_status(const std::string& name) {
    if (name == "TRANSCRIBING") return legacy_run_backfill::MessageStatus::Transcribing;
    if (name == "SENDING") return legacy_run_backfill::MessageStatus::Sending;
    if (name == "THINKING") return legacy_run_backfill::MessageStatus::Thinking;
    if (name == "TOOL_CALLING") return legacy_run_backfill::MessageStatus::ToolCalling;
    if (name == "SUCCESS") return legacy_run_backfill::MessageStatus::Success;
    if (name == "STOPPED") return legacy_run_backfill::MessageStatus::Stopped;
    if (name == "ERROR") return legacy_run_backfill::MessageStatus::Error;
    return legacy_run_backfill::MessageStatus::Error;
}

const char* status_text(legacy_run_backfill::RunStatus status) {
    using legacy_run_backfill::RunStatus;
    switch (status) {
        case RunStatus::Active: return "ACTIVE";
        case RunStatus::Stopping: return "STOPPING";
        case RunStatus::Completed: return "COMPLETED";
        case RunStatus::Stopped: return "STOPPED";
        case RunStatus::Failed: return "FAILED";
    }
    return "COMPLETED";
}

const char* reason_text(legacy_run_backfill::RunEndReason reason) {
    using legacy_run_backfill::RunEndReason;
    switch (reason) {
        case RunEndReason::ModelCompleted: return "MODEL_COMPLETED";
        case RunEndReason::UserStopped: return "USER_STOPPED";
        case RunEndReason::ProcessRecovered: return "PROCESS_RECOVERED";
        case RunEndReason::ProviderError: return "PROVIDER_ERROR";
    }
    return "MODEL_COMPLETED";
}

std::vector<LegacyMessageRecord> read_legacy_messages(Database& database,
                                                      const std::string& conversation_id) {
    const QueryResult cursor =
        database.query(kSelectLegacyMessages, {SqlValue::text_value(conversation_id)});
    const int id_index = cursor.column_index("id");
    const int parent_index = cursor.column_index("parentId");
    const int participant_index = cursor.column_index("participant");
    const int status_index = cursor.column_index("status");
    const int timestamp_index = cursor.column_index("timestamp");

    std::vector<LegacyMessageRecord> messages;
    messages.reserve(cursor.rows.size());
    for (const std::vector<SqlValue>& row : cursor.rows) {
        LegacyMessageRecord message;
        message.id = row[id_index].text;
        if (row[parent_index].kind != SqlValue::Kind::Null) {
            message.parent_id = row[parent_index].text;
        }
        message.participant = parse_participant(row[participant_index].text);
        message.status = parse_status(row[status_index].text);
        message.timestamp = row[timestamp_index].number;
        message.is_synthetic =
            starts_with(message.id, "tool_") || starts_with(message.id, "result_");
        messages.push_back(std::move(message));
    }
    return messages;
}

/** decodeSelections: strict Map<String,String> decoding; any failure yields an empty map. */
NullableSelections decode_selections(const std::optional<std::string>& raw) {
    if (!raw.has_value() || json::trim(*raw).empty()) return {};
    const json::ParseResult parsed = json::parse(*raw);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) return {};

    NullableSelections selections;
    for (const json::Member& member : parsed.value->members) {
        if (member.value.kind != json::Kind::String) return {};
        const std::optional<std::string> key =
            member.key == "null" ? std::nullopt : std::optional<std::string>(member.key);
        bool updated = false;
        for (auto& entry : selections) {
            if (entry.first == key) {
                entry.second = member.value.text;
                updated = true;
                break;
            }
        }
        if (!updated) selections.emplace_back(key, member.value.text);
    }
    return selections;
}

/** Json.encodeToString(Map<String,String>) with the null Run key spelled "null". */
std::string encode_selections(const NullableSelections& selections) {
    json::Value object;
    object.kind = json::Kind::Object;
    for (const auto& entry : selections) {
        json::Value value;
        value.kind = json::Kind::String;
        value.text = entry.second;
        object.members.push_back(
            json::Member{entry.first.has_value() ? *entry.first : std::string("null"), value});
    }
    return json::to_kotlin_string(object);
}

}  // namespace

void migrate_16_17(Database& database) {
    database.exec_sql(kAlterConversations, {});
    database.exec_sql(kCreateRuns, {});
    database.exec_sql(kIndexRunsConversation, {});
    database.exec_sql(kIndexRunsParent, {});
    database.exec_sql(kIndexRunsActiveSlot, {});

    database.exec_sql(kAlterMessagesRunId, {});
    database.exec_sql(kAlterMessagesSequence, {});
    database.exec_sql(kAlterMessagesConsumed, {});

    const QueryResult conversations = database.query(kSelectConversations, {});
    const int id_index = conversations.column_index("id");
    const int selected_index = conversations.column_index("selectedBranchesJson");
    for (const std::vector<SqlValue>& row : conversations.rows) {
        const std::string conversation_id = row[id_index].text;
        std::optional<std::string> selected_raw;
        if (row[selected_index].kind != SqlValue::Kind::Null) {
            selected_raw = row[selected_index].text;
        }

        const std::vector<LegacyMessageRecord> messages =
            read_legacy_messages(database, conversation_id);
        const legacy_run_backfill::PlanResult plan =
            legacy_run_backfill::plan(conversation_id, messages);
        if (!plan.ok) {
            throw MigrationException{"IllegalArgumentException", plan.error};
        }

        for (const legacy_run_backfill::PlannedLegacyRun& run : plan.plan.runs) {
            std::vector<SqlValue> args;
            args.push_back(SqlValue::text_value(run.id));
            args.push_back(SqlValue::text_value(run.conversation_id));
            args.push_back(run.parent_run_id.has_value()
                               ? SqlValue::text_value(*run.parent_run_id)
                               : SqlValue::null());
            args.push_back(SqlValue::text_value(status_text(run.status)));
            args.push_back(SqlValue::long_value(run.started_at));
            args.push_back(SqlValue::long_value(run.ended_at));
            args.push_back(SqlValue::long_value(run.ended_at));
            args.push_back(SqlValue::text_value(reason_text(run.end_reason)));
            args.push_back(SqlValue::int_value(run.legacy_ambiguous ? 1 : 0));
            database.exec_sql(kInsertRun, args);
        }

        for (const legacy_run_backfill::PlannedMessageAssignment& assignment :
             plan.plan.assignments) {
            std::vector<SqlValue> args;
            args.push_back(SqlValue::text_value(assignment.run_id));
            args.push_back(SqlValue::long_value(assignment.run_sequence));
            args.push_back(assignment.consumed_at_pass.has_value()
                               ? SqlValue::int_value(*assignment.consumed_at_pass)
                               : SqlValue::null());
            args.push_back(SqlValue::text_value(assignment.message_id));
            database.exec_sql(kUpdateMessageAssignment, args);
        }

        const legacy_run_backfill::SelectionResult selected =
            legacy_run_backfill::selected_run_branches(messages, plan.plan,
                                                       decode_selections(selected_raw));
        if (!selected.ok) {
            throw MigrationException{"IllegalArgumentException", selected.error};
        }
        if (!selected.selections.empty()) {
            std::vector<SqlValue> args;
            args.push_back(SqlValue::text_value(encode_selections(selected.selections)));
            args.push_back(SqlValue::text_value(conversation_id));
            database.exec_sql(kUpdateConversationSelections, args);
        }
    }

    const QueryResult missing_cursor = database.query(kCountUnassigned, {});
    if (missing_cursor.rows.empty() || missing_cursor.rows.front().empty()) {
        throw MigrationException{"IllegalStateException", "Check failed."};
    }
    const long long missing_assignments = missing_cursor.rows.front().front().number;
    if (missing_assignments != 0) {
        throw MigrationException{
            "IllegalStateException",
            "v16-to-v17 migration left " + std::to_string(missing_assignments) +
                " messages without a Run"};
    }

    database.exec_sql(kCreateMessagesNew, {});
    database.exec_sql(kCopyMessages, {});
    database.exec_sql(kDropMessages, {});
    database.exec_sql(kRenameMessages, {});
    database.exec_sql(kIndexMessagesConversation, {});
    database.exec_sql(kIndexMessagesRun, {});
}

}  // namespace agora::core::migration_16_17
