// Port of data/local/migration/Migration18To19.kt.
//
// The migration reads both v18 projections, asks the shared v18 edge-repair planner for the
// message parent updates and applies them in planner order. SQL text and bind-argument order are
// part of the migration contract and are reproduced literally.
#include "agora/core/migration_18_19.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/v18_regeneration_repair.hpp"

namespace agora::core::migration_18_19 {
namespace {

using v18_regeneration_repair::MessageRecord;
using v18_regeneration_repair::Participant;
using v18_regeneration_repair::RunRecord;

constexpr char kSelectRuns[] = "SELECT id, parentRunId, legacyAmbiguous FROM runs";
constexpr char kSelectMessages[] =
    "SELECT id, parentId, participant, runId, runSequence FROM messages";
constexpr char kUpdateMessageParent[] = "UPDATE messages SET parentId = ? WHERE id = ?";

Participant parse_participant(const std::string& name) {
    if (name == "USER") return Participant::User;
    if (name == "MODEL") return Participant::Model;
    if (name == "ERROR") return Participant::Error;
    return Participant::Error;
}

std::vector<RunRecord> read_runs(Database& database) {
    const QueryResult cursor = database.query(kSelectRuns, {});
    const int id_index = cursor.column_index("id");
    const int parent_index = cursor.column_index("parentRunId");
    const int ambiguous_index = cursor.column_index("legacyAmbiguous");

    std::vector<RunRecord> runs;
    runs.reserve(cursor.rows.size());
    for (const std::vector<SqlValue>& row : cursor.rows) {
        RunRecord run;
        run.id = row[id_index].text;
        if (row[parent_index].kind != SqlValue::Kind::Null) {
            run.parent_run_id = row[parent_index].text;
        }
        run.legacy_ambiguous = row[ambiguous_index].number != 0;
        runs.push_back(std::move(run));
    }
    return runs;
}

std::vector<MessageRecord> read_messages(Database& database) {
    const QueryResult cursor = database.query(kSelectMessages, {});
    const int id_index = cursor.column_index("id");
    const int parent_index = cursor.column_index("parentId");
    const int participant_index = cursor.column_index("participant");
    const int run_id_index = cursor.column_index("runId");
    const int sequence_index = cursor.column_index("runSequence");

    std::vector<MessageRecord> messages;
    messages.reserve(cursor.rows.size());
    for (const std::vector<SqlValue>& row : cursor.rows) {
        MessageRecord message;
        message.id = row[id_index].text;
        if (row[parent_index].kind != SqlValue::Kind::Null) {
            message.parent_id = row[parent_index].text;
        }
        message.participant = parse_participant(row[participant_index].text);
        message.run_id = row[run_id_index].text;
        message.run_sequence = row[sequence_index].number;
        messages.push_back(std::move(message));
    }
    return messages;
}

}  // namespace

void migrate_18_19(Database& database) {
    const std::vector<RunRecord> runs = read_runs(database);
    const std::vector<MessageRecord> messages = read_messages(database);
    const auto updates = v18_regeneration_repair::plan(runs, messages);
    for (const auto& [message_id, parent_id] : updates) {
        database.exec_sql(kUpdateMessageParent,
                          {SqlValue::text_value(parent_id), SqlValue::text_value(message_id)});
    }
}

}  // namespace agora::core::migration_18_19
