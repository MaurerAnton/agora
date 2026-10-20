// Port of data/local/migration/Migration17To18.kt.
//
// The SQL text, statement order and bind-argument types are part of the Room data-shape contract,
// so they are reproduced literally. Cursor plumbing is the platform boundary: the port asks the
// Database interface for the same projections the Kotlin migration reads.
#include "agora/core/migration_17_18.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/regeneration_tree_repair.hpp"

namespace agora::core::migration_17_18 {
namespace {

using regeneration_tree_repair::Participant;
using regeneration_tree_repair::V17MessageRecord;
using regeneration_tree_repair::V17RunRecord;
using Selection = std::pair<std::optional<std::string>, std::string>;
using Selections = std::vector<Selection>;

constexpr char kSelectConversations[] =
    "SELECT id, selectedBranchesJson, selectedRunBranchesJson FROM conversations";

constexpr char kSelectRuns[] = R"(SELECT id, parentRunId, startedAt
FROM runs
WHERE conversationId = ?)";

constexpr char kSelectMessages[] = R"(SELECT id, parentId, text, images, attachmentMeta, participant, timestamp, runId, runSequence
FROM messages
WHERE conversationId = ?)";

constexpr char kUpdateRunParent[] =
    "UPDATE runs SET parentRunId = ?, legacyAmbiguous = 1 WHERE id = ?";
constexpr char kUpdateMessageParent[] = "UPDATE messages SET parentId = ? WHERE id = ?";
constexpr char kDeleteEmbeddings[] = "DELETE FROM embeddings WHERE messageId = ?";
constexpr char kDeleteMessage[] = "DELETE FROM messages WHERE id = ?";
constexpr char kUpdateRunSequence[] = "UPDATE messages SET runSequence = ? WHERE id = ?";

constexpr char kUpdateConversations[] = R"(UPDATE conversations
SET selectedBranchesJson = ?, selectedRunBranchesJson = ?
WHERE id = ?)";

Participant parse_participant(const std::string& name) {
    if (name == "USER") return Participant::User;
    if (name == "MODEL") return Participant::Model;
    if (name == "ERROR") return Participant::Error;
    return Participant::Error;
}

/** imageCount: strict List<String> JSON, else the literal "|||" split fallback. */
int image_count(const std::string& raw) {
    if (raw.empty()) return 0;
    const json::ParseResult parsed = json::parse(raw);
    if (parsed.value.has_value() && parsed.value->kind == json::Kind::Array) {
        bool all_strings = true;
        for (const json::Value& element : parsed.value->elements) {
            if (element.kind != json::Kind::String) {
                all_strings = false;
                break;
            }
        }
        if (all_strings) return static_cast<int>(parsed.value->elements.size());
    }
    int count = 1;
    std::size_t position = 0;
    while ((position = raw.find("|||", position)) != std::string::npos) {
        ++count;
        position += 3;
    }
    return count;
}

std::vector<V17RunRecord> read_runs(Database& database, const std::string& conversation_id) {
    const QueryResult cursor = database.query(kSelectRuns, {SqlValue::text_value(conversation_id)});
    const int id_index = cursor.column_index("id");
    const int parent_index = cursor.column_index("parentRunId");
    const int started_index = cursor.column_index("startedAt");

    std::vector<V17RunRecord> runs;
    runs.reserve(cursor.rows.size());
    for (const std::vector<SqlValue>& row : cursor.rows) {
        V17RunRecord run;
        run.id = row[id_index].text;
        if (row[parent_index].kind != SqlValue::Kind::Null) {
            run.parent_run_id = row[parent_index].text;
        }
        run.started_at = row[started_index].number;
        runs.push_back(std::move(run));
    }
    return runs;
}

std::vector<V17MessageRecord> read_messages(Database& database,
                                            const std::string& conversation_id) {
    const QueryResult cursor =
        database.query(kSelectMessages, {SqlValue::text_value(conversation_id)});
    const int id_index = cursor.column_index("id");
    const int parent_index = cursor.column_index("parentId");
    const int text_index = cursor.column_index("text");
    const int images_index = cursor.column_index("images");
    const int attachment_index = cursor.column_index("attachmentMeta");
    const int participant_index = cursor.column_index("participant");
    const int timestamp_index = cursor.column_index("timestamp");
    const int run_id_index = cursor.column_index("runId");
    const int run_sequence_index = cursor.column_index("runSequence");

    std::vector<V17MessageRecord> messages;
    messages.reserve(cursor.rows.size());
    for (const std::vector<SqlValue>& row : cursor.rows) {
        V17MessageRecord message;
        message.id = row[id_index].text;
        if (row[parent_index].kind != SqlValue::Kind::Null) {
            message.parent_id = row[parent_index].text;
        }
        message.participant = parse_participant(row[participant_index].text);
        message.timestamp = row[timestamp_index].number;
        message.run_id = row[run_id_index].text;
        message.run_sequence = row[run_sequence_index].number;
        const std::string text = row[text_index].text;
        const std::string images = row[images_index].text;
        const std::optional<std::string> attachment =
            row[attachment_index].kind == SqlValue::Kind::Null
                ? std::nullopt
                : std::optional<std::string>(row[attachment_index].text);
        message.input_fingerprint = regeneration_tree_repair::regeneration_input_fingerprint(
            text, image_count(images), attachment);
        messages.push_back(std::move(message));
    }
    return messages;
}

/** decodeSelections: strict Map<String,String> decoding; any failure yields an empty map. */
Selections decode_selections(const std::optional<std::string>& raw) {
    if (!raw.has_value() || json::trim(*raw).empty()) return {};
    const json::ParseResult parsed = json::parse(*raw);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) return {};

    Selections selections;
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

/** Json.encodeToString(Map<String,String>) with the null selection key spelled "null". */
std::string encode_selections(const Selections& selections) {
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

void migrate_17_18(Database& database) {
    const QueryResult conversations = database.query(kSelectConversations, {});
    const int id_index = conversations.column_index("id");
    const int message_selections_index = conversations.column_index("selectedBranchesJson");
    const int run_selections_index = conversations.column_index("selectedRunBranchesJson");

    for (const std::vector<SqlValue>& row : conversations.rows) {
        const std::string conversation_id = row[id_index].text;
        std::optional<std::string> message_selections_raw;
        if (row[message_selections_index].kind != SqlValue::Kind::Null) {
            message_selections_raw = row[message_selections_index].text;
        }
        std::optional<std::string> run_selections_raw;
        if (row[run_selections_index].kind != SqlValue::Kind::Null) {
            run_selections_raw = row[run_selections_index].text;
        }

        const std::vector<V17RunRecord> runs = read_runs(database, conversation_id);
        const std::vector<V17MessageRecord> messages = read_messages(database, conversation_id);
        const regeneration_tree_repair::Plan plan = regeneration_tree_repair::plan(
            runs, messages, decode_selections(message_selections_raw),
            decode_selections(run_selections_raw));
        if (plan.inferred_run_ids.empty()) continue;

        for (const auto& [run_id, parent_run_id] : plan.run_parent_updates) {
            database.exec_sql(kUpdateRunParent,
                              {SqlValue::text_value(parent_run_id), SqlValue::text_value(run_id)});
        }
        for (const auto& [message_id, parent_id] : plan.message_parent_updates) {
            database.exec_sql(kUpdateMessageParent,
                              {SqlValue::text_value(parent_id), SqlValue::text_value(message_id)});
        }
        for (const std::string& message_id : plan.deleted_message_ids) {
            database.exec_sql(kDeleteEmbeddings, {SqlValue::text_value(message_id)});
            database.exec_sql(kDeleteMessage, {SqlValue::text_value(message_id)});
        }
        for (const auto& [message_id, run_sequence] : plan.run_sequence_updates) {
            database.exec_sql(kUpdateRunSequence,
                              {SqlValue::long_value(run_sequence), SqlValue::text_value(message_id)});
        }
        database.exec_sql(kUpdateConversations,
                          {SqlValue::text_value(encode_selections(plan.message_selections)),
                           SqlValue::text_value(encode_selections(plan.run_selections)),
                           SqlValue::text_value(conversation_id)});
    }
}

}  // namespace agora::core::migration_17_18
