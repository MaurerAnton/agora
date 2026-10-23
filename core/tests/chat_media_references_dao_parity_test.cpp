// Golden parity test for data/local/ChatMediaReferencesDao.kt.
//
// The stand runs the Kotlin interface against a real SQLite database through Room's replaced
// annotation processor; this test runs the C++ ChatMediaReferencesDao (which owns the transcribed
// SQL) against the same schema (platform/schema.sql, shipped with the stand). All five members
// are driven, and the run ends with a dump of the observed tables to prove the reads do not
// mutate the store.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "agora/core/chat_media_references_dao.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace media = agora::core::media_references;
namespace json = agora::core::json;

using agora::core::KotlinError;
using agora::core::SqlArgs;
using agora::core::SqlError;
using agora::core::SqlRow;
using agora::core::SqlValue;
using media::ChatMediaReferencesDao;
using media::ConversationDraftAttachmentReference;
using media::MessageAttachmentReference;
using media::MessageToolMediaReference;
using media::NewChatDraftAttachmentReference;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null_element(const json::Value& value) {
    return value.kind == json::Kind::Literal && value.text == "null";
}

std::string primitive_content(const json::Value& value) {
    if (value.kind == json::Kind::String) return value.text;
    if (value.kind == json::Kind::Literal) return value.text;
    scenario_fail("expected a JSON primitive");
}

std::string required_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind == json::Kind::Array || value->kind == json::Kind::Object) {
        throw KotlinError("IllegalStateException", "missing string " + std::string(name));
    }
    return primitive_content(*value);
}

std::optional<std::string> optional_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_null_element(*value)) return std::nullopt;
    return primitive_content(*value);
}

std::int64_t required_long(const json::Value& object, std::string_view name) {
    const std::string content = required_string(object, name);
    std::size_t consumed = 0;
    try {
        const long long parsed = std::stoll(content, &consumed);
        if (consumed != content.size()) throw std::invalid_argument("trailing");
        return parsed;
    } catch (const std::exception&) {
        throw KotlinError("NumberFormatException", "For input string: \"" + content + "\"");
    }
}

std::optional<std::int64_t> optional_long(const json::Value& object, std::string_view name) {
    const std::optional<std::string> content = optional_string(object, name);
    if (!content.has_value()) return std::nullopt;
    std::size_t consumed = 0;
    try {
        const long long parsed = std::stoll(*content, &consumed);
        if (consumed != content->size()) throw std::invalid_argument("trailing");
        return parsed;
    } catch (const std::exception&) {
        throw KotlinError("NumberFormatException", "For input string: \"" + *content + "\"");
    }
}

int required_int(const json::Value& object, std::string_view name) {
    return static_cast<int>(required_long(object, name));
}

// ---- json builders ------------------------------------------------------------------------

json::Value json_null() {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = "null";
    return value;
}

json::Value json_string(std::string_view text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::string(text);
    return value;
}

json::Value json_long(std::int64_t number) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::to_string(number);
    return value;
}

json::Value json_array(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        json::Member entry;
        entry.key = std::move(member.first);
        entry.value = std::move(member.second);
        value.members.push_back(std::move(entry));
    }
    return value;
}

json::Value optional_json_string(const std::optional<std::string>& text) {
    if (!text.has_value()) return json_null();
    return json_string(*text);
}

json::Value string_list_json(const std::vector<std::string>& values) {
    std::vector<json::Value> out;
    for (const std::string& value : values) out.push_back(json_string(value));
    return json_array(std::move(out));
}

// ---- sqlite executor (SqlExecutor boundary) -----------------------------------------------

class SqliteExecutor : public agora::core::SqlExecutor {
public:
    explicit SqliteExecutor(const std::filesystem::path& schema_file) {
        if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
            scenario_fail(std::string("cannot open sqlite: ") + sqlite3_errmsg(db_));
        }
        exec_raw("PRAGMA foreign_keys = ON");
        const std::string schema = read_file(schema_file);
        char* error = nullptr;
        if (sqlite3_exec(db_, schema.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "schema failed";
            sqlite3_free(error);
            scenario_fail("schema: " + message);
        }
    }

    ~SqliteExecutor() override {
        if (db_ != nullptr) sqlite3_close(db_);
    }

    std::int64_t execute(const std::string& sql, const SqlArgs& args) override {
        sqlite3_stmt* statement = prepare(sql);
        try {
            bind(statement, args);
        } catch (...) {
            sqlite3_finalize(statement);
            throw;
        }
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_DONE) {
            const std::int64_t changed = sqlite3_changes(db_);
            sqlite3_finalize(statement);
            return changed;
        }
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(statement);
        throw SqlError("sqlite: " + message + " in " + sql);
    }

    std::vector<SqlRow> query(const std::string& sql, const SqlArgs& args) override {
        sqlite3_stmt* statement = prepare(sql);
        try {
            bind(statement, args);
        } catch (...) {
            sqlite3_finalize(statement);
            throw;
        }
        std::vector<SqlRow> rows;
        for (;;) {
            const int rc = sqlite3_step(statement);
            if (rc == SQLITE_ROW) {
                rows.push_back(read_row(statement));
                continue;
            }
            if (rc == SQLITE_DONE) break;
            const std::string message = sqlite3_errmsg(db_);
            sqlite3_finalize(statement);
            throw SqlError("sqlite: " + message + " in " + sql);
        }
        sqlite3_finalize(statement);
        return rows;
    }

    std::int64_t last_insert_rowid() override { return sqlite3_last_insert_rowid(db_); }

    void begin() override { exec_raw("BEGIN"); }
    void commit() override { exec_raw("COMMIT"); }
    void rollback() override { exec_raw("ROLLBACK"); }

private:
    void exec_raw(const std::string& sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "exec failed";
            sqlite3_free(error);
            scenario_fail("sqlite: " + message + " in " + sql);
        }
    }

    sqlite3_stmt* prepare(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr);
        if (rc != SQLITE_OK) {
            throw SqlError(std::string("sqlite prepare: ") + sqlite3_errmsg(db_) + " in " + sql);
        }
        return statement;
    }

    void bind(sqlite3_stmt* statement, const SqlArgs& args) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const int index = static_cast<int>(i + 1);
            const SqlValue& value = args[i];
            int rc = SQLITE_OK;
            switch (value.kind) {
                case SqlValue::Kind::Null:
                    rc = sqlite3_bind_null(statement, index);
                    break;
                case SqlValue::Kind::Text:
                    rc = sqlite3_bind_text(statement, index, value.text.data(),
                                           static_cast<int>(value.text.size()), SQLITE_TRANSIENT);
                    break;
                case SqlValue::Kind::Integer:
                    rc = sqlite3_bind_int64(statement, index, value.integer);
                    break;
                case SqlValue::Kind::Real:
                    rc = sqlite3_bind_double(statement, index, value.real);
                    break;
                case SqlValue::Kind::Blob: {
                    const void* data = value.blob.empty()
                                           ? static_cast<const void*>("")
                                           : static_cast<const void*>(value.blob.data());
                    rc = sqlite3_bind_blob(statement, index, data,
                                           static_cast<int>(value.blob.size()), SQLITE_TRANSIENT);
                    break;
                }
            }
            if (rc != SQLITE_OK) throw SqlError(std::string("sqlite bind: ") + sqlite3_errmsg(db_));
        }
    }

    static SqlRow read_row(sqlite3_stmt* statement) {
        SqlRow row;
        const int columns = sqlite3_column_count(statement);
        for (int i = 0; i < columns; ++i) {
            SqlValue value;
            switch (sqlite3_column_type(statement, i)) {
                case SQLITE_NULL:
                    value.kind = SqlValue::Kind::Null;
                    break;
                case SQLITE_INTEGER:
                    value.kind = SqlValue::Kind::Integer;
                    value.integer = sqlite3_column_int64(statement, i);
                    break;
                case SQLITE_FLOAT:
                    value.kind = SqlValue::Kind::Real;
                    value.real = sqlite3_column_double(statement, i);
                    break;
                case SQLITE_TEXT: {
                    value.kind = SqlValue::Kind::Text;
                    const auto* text = sqlite3_column_text(statement, i);
                    const int length = sqlite3_column_bytes(statement, i);
                    value.text.assign(reinterpret_cast<const char*>(text),
                                      static_cast<std::size_t>(length));
                    break;
                }
                case SQLITE_BLOB: {
                    value.kind = SqlValue::Kind::Blob;
                    const auto* data = static_cast<const std::uint8_t*>(
                        sqlite3_column_blob(statement, i));
                    const int length = sqlite3_column_bytes(statement, i);
                    value.blob.assign(data, data + length);
                    break;
                }
            }
            row.push_back(std::move(value));
        }
        return row;
    }

    sqlite3* db_ = nullptr;
};

// ---- seeding -------------------------------------------------------------------------------

void seed(SqliteExecutor& sql, const json::Value& root) {
    const json::Value* seed_object = field(root, "seed");
    if (seed_object == nullptr) return;
    if (const json::Value* conversations = field(*seed_object, "conversations");
        conversations != nullptr) {
        for (const json::Value& row : conversations->elements) {
            const std::optional<std::string> attachments =
                optional_string(row, "draftAttachments");
            sql.execute(
                "INSERT INTO conversations (id, title, lastUpdated, dataChangedAt, "
                "selectedBranchesJson, systemPromptId, modelId, taskId, origin, graduated, "
                "draftText, draftAttachments, selectedRunBranchesJson, hasUnreadGeneration, "
                "isPinned) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 SqlValue::text_value(optional_string(row, "title").value_or("")),
                 SqlValue::integer_value(optional_long(row, "lastUpdated").value_or(0)),
                 SqlValue::integer_value(optional_long(row, "dataChangedAt").value_or(0)),
                 SqlValue::null(), SqlValue::null(), SqlValue::null(), SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "origin").value_or("user")),
                 SqlValue::integer_value(0),
                 SqlValue::text_value(optional_string(row, "draftText").value_or("")),
                 attachments.has_value() ? SqlValue::text_value(*attachments) : SqlValue::null(),
                 SqlValue::null(), SqlValue::integer_value(0), SqlValue::integer_value(0)});
        }
    }
    if (const json::Value* runs = field(*seed_object, "runs"); runs != nullptr) {
        for (const json::Value& row : runs->elements) {
            const std::int64_t started_at = optional_long(row, "startedAt").value_or(0);
            const std::optional<std::string> parent = optional_string(row, "parentRunId");
            const std::optional<std::int64_t> slot = optional_long(row, "activeSlot");
            const std::optional<std::int64_t> stopped = optional_long(row, "stopRequestedAt");
            const std::optional<std::int64_t> ended = optional_long(row, "endedAt");
            const std::optional<std::string> reason = optional_string(row, "endReason");
            sql.execute(
                "INSERT INTO runs (id, conversationId, parentRunId, status, activeSlot, startedAt, "
                "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, "
                "legacyAmbiguous) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 SqlValue::text_value(required_string(row, "conversationId")),
                 parent.has_value() ? SqlValue::text_value(*parent) : SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "status").value_or("COMPLETED")),
                 slot.has_value() ? SqlValue::integer_value(*slot) : SqlValue::null(),
                 SqlValue::integer_value(started_at),
                 SqlValue::integer_value(
                     optional_long(row, "lastCheckpointAt").value_or(started_at)),
                 stopped.has_value() ? SqlValue::integer_value(*stopped) : SqlValue::null(),
                 ended.has_value() ? SqlValue::integer_value(*ended) : SqlValue::null(),
                 reason.has_value() ? SqlValue::text_value(*reason) : SqlValue::null(),
                 SqlValue::integer_value(optional_long(row, "currentPass").value_or(0)),
                 SqlValue::integer_value(0)});
        }
    }
    if (const json::Value* messages = field(*seed_object, "messages"); messages != nullptr) {
        for (const json::Value& row : messages->elements) {
            std::string images;
            if (const json::Value* list = field(row, "images");
                list != nullptr && list->kind == json::Kind::Array) {
                images = json::to_kotlin_string(*list);
            } else if (const std::optional<std::string> raw = optional_string(row, "imagesRaw");
                       raw.has_value()) {
                images = *raw;
            } else {
                images = "[]";
            }
            const std::optional<std::string> attachment = optional_string(row, "attachmentMeta");
            const std::optional<std::string> tool_json = optional_string(row, "toolCallJson");
            const std::optional<std::string> parent = optional_string(row, "parentId");
            const std::optional<std::string> model = optional_string(row, "modelName");
            sql.execute(
                "INSERT INTO messages (id, conversationId, parentId, text, images, thoughts, "
                "thoughtTitle, tokenCount, inputTokenCount, cachedInputTokenCount, "
                "cacheWriteInputTokenCount, uncachedInputTokenCount, outputTokenCount, "
                "reasoningTokenCount, generationDurationMs, status, participant, timestamp, "
                "thoughtTimeMs, modelName, toolCallJson, attachmentMeta, runId, runSequence, "
                "consumedAtPass, sourceJson) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 SqlValue::text_value(required_string(row, "conversationId")),
                 parent.has_value() ? SqlValue::text_value(*parent) : SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "text").value_or("")),
                 SqlValue::text_value(images),
                 SqlValue::null(), SqlValue::null(), SqlValue::integer_value(0),
                 SqlValue::null(), SqlValue::null(), SqlValue::null(), SqlValue::null(),
                 SqlValue::null(), SqlValue::null(), SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "status").value_or("SUCCESS")),
                 SqlValue::text_value(optional_string(row, "participant").value_or("MODEL")),
                 SqlValue::integer_value(optional_long(row, "timestamp").value_or(0)),
                 SqlValue::null(),
                 model.has_value() ? SqlValue::text_value(*model) : SqlValue::null(),
                 tool_json.has_value() ? SqlValue::text_value(*tool_json) : SqlValue::null(),
                 attachment.has_value() ? SqlValue::text_value(*attachment) : SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "runId").value_or("r1")),
                 SqlValue::integer_value(optional_long(row, "runSequence").value_or(-1)),
                 SqlValue::null(), SqlValue::null()});
        }
    }
    if (const json::Value* persist = field(*seed_object, "newChatPersist"); persist != nullptr) {
        for (const json::Value& row : persist->elements) {
            const std::optional<std::string> model = optional_string(row, "modelId");
            const std::optional<std::string> prompt = optional_string(row, "systemPromptId");
            const std::optional<std::string> settings =
                optional_string(row, "conversationSettingsJson");
            const std::optional<std::string> attachments =
                optional_string(row, "draftAttachments");
            sql.execute(
                "INSERT INTO new_chat_persist (id, modelId, systemPromptId, "
                "conversationSettingsJson, draftText, draftAttachments) VALUES (?, ?, ?, ?, ?, ?)",
                {SqlValue::integer_value(optional_long(row, "id").value_or(0)),
                 model.has_value() ? SqlValue::text_value(*model) : SqlValue::null(),
                 prompt.has_value() ? SqlValue::text_value(*prompt) : SqlValue::null(),
                 settings.has_value() ? SqlValue::text_value(*settings) : SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "draftText").value_or("")),
                 attachments.has_value() ? SqlValue::text_value(*attachments)
                                          : SqlValue::null()});
        }
    }
}

// ---- rendering ----------------------------------------------------------------------------

json::Value message_attachment_reference_json(const MessageAttachmentReference& entity) {
    return json_object({{"id", json_string(entity.id)},
                        {"images", string_list_json(entity.images)},
                        {"attachmentMeta", optional_json_string(entity.attachment_meta)}});
}

json::Value message_tool_media_reference_json(const MessageToolMediaReference& entity) {
    return json_object({{"id", json_string(entity.id)},
                        {"toolCallJson", json_string(entity.tool_call_json)}});
}

json::Value conversation_draft_attachment_reference_json(
    const ConversationDraftAttachmentReference& entity) {
    return json_object({{"id", json_string(entity.id)},
                        {"draftAttachments", json_string(entity.draft_attachments)}});
}

json::Value new_chat_draft_attachment_reference_json(
    const NewChatDraftAttachmentReference& entity) {
    return json_object({{"draftAttachments", json_string(entity.draft_attachments)}});
}

// ---- driver --------------------------------------------------------------------------------

json::Value dispatch(const std::string& op, const json::Value& step, ChatMediaReferencesDao& dao) {
    if (op == "getMessageAttachmentReferencesPage") {
        const std::vector<MessageAttachmentReference> rows =
            dao.get_message_attachment_references_page(optional_string(step, "afterId"),
                                                       required_int(step, "limit"),
                                                       optional_string(step, "pathToken"));
        std::vector<json::Value> out;
        for (const MessageAttachmentReference& row : rows) {
            out.push_back(message_attachment_reference_json(row));
        }
        return json_array(std::move(out));
    }
    if (op == "getConversationMessageAttachmentReferencesPage") {
        const std::vector<MessageAttachmentReference> rows =
            dao.get_conversation_message_attachment_references_page(
                required_string(step, "conversationId"), optional_string(step, "afterId"),
                required_int(step, "limit"));
        std::vector<json::Value> out;
        for (const MessageAttachmentReference& row : rows) {
            out.push_back(message_attachment_reference_json(row));
        }
        return json_array(std::move(out));
    }
    if (op == "getMessageToolMediaReferencesPage") {
        const std::vector<MessageToolMediaReference> rows =
            dao.get_message_tool_media_references_page(optional_string(step, "afterId"),
                                                       required_int(step, "limit"));
        std::vector<json::Value> out;
        for (const MessageToolMediaReference& row : rows) {
            out.push_back(message_tool_media_reference_json(row));
        }
        return json_array(std::move(out));
    }
    if (op == "getConversationDraftAttachmentReferencesPage") {
        const std::vector<ConversationDraftAttachmentReference> rows =
            dao.get_conversation_draft_attachment_references_page(
                optional_string(step, "afterId"), required_int(step, "limit"),
                optional_string(step, "pathToken"));
        std::vector<json::Value> out;
        for (const ConversationDraftAttachmentReference& row : rows) {
            out.push_back(conversation_draft_attachment_reference_json(row));
        }
        return json_array(std::move(out));
    }
    if (op == "getNewChatDraftAttachmentReference") {
        const std::optional<NewChatDraftAttachmentReference> row =
            dao.get_new_chat_draft_attachment_reference(optional_string(step, "pathToken"));
        if (!row.has_value()) return json_null();
        return new_chat_draft_attachment_reference_json(*row);
    }
    throw KotlinError("IllegalStateException", "unknown operation " + op);
}

// ---- table dump ----------------------------------------------------------------------------

json::Value query_rows(SqliteExecutor& sql, const std::string& query,
                       json::Value (*render)(const SqlRow&)) {
    std::vector<json::Value> rows;
    for (const SqlRow& row : sql.query(query, {})) rows.push_back(render(row));
    return json_array(std::move(rows));
}

json::Value conversation_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"draftAttachments", optional_json_string(row[1].text_or_null())}});
}

json::Value message_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"conversationId", json_string(row[1].as_text())},
                        {"images", json_string(row[2].as_text())},
                        {"attachmentMeta", optional_json_string(row[3].text_or_null())},
                        {"toolCallJson", optional_json_string(row[4].text_or_null())}});
}

json::Value persist_row_json(const SqlRow& row) {
    return json_object({{"id", json_long(row[0].as_integer())},
                        {"modelId", optional_json_string(row[1].text_or_null())},
                        {"systemPromptId", optional_json_string(row[2].text_or_null())},
                        {"conversationSettingsJson",
                         optional_json_string(row[3].text_or_null())},
                        {"draftText", json_string(row[4].as_text())},
                        {"draftAttachments", optional_json_string(row[5].text_or_null())}});
}

json::Value dump_tables(SqliteExecutor& sql) {
    return json_object(
        {{"conversations",
          query_rows(sql, "SELECT id, draftAttachments FROM conversations ORDER BY id",
                     conversation_row_json)},
         {"messages",
          query_rows(sql,
                     "SELECT id, conversationId, images, attachmentMeta, toolCallJson FROM "
                     "messages ORDER BY id",
                     message_row_json)},
         {"new_chat_persist",
          query_rows(sql,
                     "SELECT id, modelId, systemPromptId, conversationSettingsJson, draftText, "
                     "draftAttachments FROM new_chat_persist ORDER BY id",
                     persist_row_json)}});
}

std::string run_scenario(const std::filesystem::path& scenario) {
    const std::filesystem::path root_dir = AGORA_FIXTURE_CHAT_MEDIA_REFERENCES_DAO_DIR;
    const json::ParseResult parsed = json::parse(read_file(scenario));
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        scenario_fail(scenario.stem().string() + ": scenario must be an object");
    }
    const json::Value& root = *parsed.value;

    SqliteExecutor sql(root_dir / "platform" / "schema.sql");
    seed(sql, root);
    ChatMediaReferencesDao dao(sql);

    std::vector<json::Value> steps;
    const json::Value* step_list = field(root, "steps");
    if (step_list != nullptr && step_list->kind == json::Kind::Array) {
        for (const json::Value& element : step_list->elements) {
            const std::string op = required_string(element, "op");
            json::Value result;
            try {
                result = dispatch(op, element, dao);
            } catch (const KotlinError& failure) {
                result = json_object({{"error", json_string(failure.class_name())},
                                      {"message", json_string(failure.what())}});
            } catch (const SqlError& failure) {
                result = json_object({{"error", json_string("SQLException")},
                                      {"message", json_string(failure.what())}});
            }
            steps.push_back(json_object({{"op", json_string(op)}, {"result", std::move(result)}}));
        }
    }

    json::Value trace = json_object({{"steps", json_array(std::move(steps))},
                                     {"tables", dump_tables(sql)}});
    return json::to_kotlin_string(trace);
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 80 ? at - 80 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(320, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << ", expected "
            << expected.size() << ")\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(chat_media_references_dao_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CHAT_MEDIA_REFERENCES_DAO_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
            expected.pop_back();
        }
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
