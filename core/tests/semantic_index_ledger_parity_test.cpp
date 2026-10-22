// Golden parity test for the semantic index ledger (data/local/SemanticIndexLedger.kt).
//
// The stand runs the Kotlin ledger against a real SQLite database through Room's replaced
// annotation processor; this test runs the C++ port against the same schema (platform/schema.sql,
// shipped with the stand) through a sqlite3 implementation of SqlExecutor. Each scenario step
// records its result (or the Kotlin validation error it raises) and the run ends with a dump of
// the five observed tables, so the traces must match byte for byte.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "agora/core/json.hpp"
#include "agora/core/semantic_index_ledger.hpp"
#include "check.hpp"

namespace {

namespace ledger = agora::core::semantic_index;
namespace json = agora::core::json;

using ledger::EmbeddingEntry;
using ledger::LedgerEntry;
using ledger::ReconcileIndexableMessage;
using ledger::SemanticModelSnapshot;
using ledger::SqlArgs;
using ledger::SqlRow;
using ledger::SqlValue;
using ledger::WorkEntry;

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

/** Kotlin JsonPrimitive.content for a primitive: String text or the raw literal. */
std::string primitive_content(const json::Value& value) {
    if (value.kind == json::Kind::String) return value.text;
    if (value.kind == json::Kind::Literal) return value.text;
    scenario_fail("expected a JSON primitive");
}

/** JsonObject.string(name): content or the Kotlin `error("missing string ...")` failure. */
std::string required_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind == json::Kind::Array || value->kind == json::Kind::Object) {
        throw ledger::KotlinError("IllegalStateException",
                                  "missing string " + std::string(name));
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
        throw ledger::KotlinError("NumberFormatException", "For input string: \"" + content + "\"");
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
        throw ledger::KotlinError("NumberFormatException",
                                  "For input string: \"" + *content + "\"");
    }
}

/** JsonObject.boolean(name, default): toBooleanStrictOrNull() with the same default. */
bool optional_boolean(const json::Value& object, std::string_view name, bool default_value) {
    const std::optional<std::string> content = optional_string(object, name);
    if (!content.has_value()) return default_value;
    if (*content == "true") return true;
    if (*content == "false") return false;
    return default_value;
}

std::vector<std::string> string_array(const json::Value& object, std::string_view name) {
    std::vector<std::string> out;
    const json::Value* value = field(object, name);
    if (value == nullptr) return out;
    if (value->kind != json::Kind::Array) scenario_fail(std::string(name) + " must be an array");
    for (const json::Value& element : value->elements) out.push_back(primitive_content(element));
    return out;
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

json::Value json_bool(bool flag) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = flag ? "true" : "false";
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

std::string hex_lower(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::vector<std::uint8_t> hex_to_bytes(std::string_view hex) {
    if (hex.size() % 2 != 0) scenario_fail("hex text with an odd length");
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const std::string byte_text(hex.substr(i, 2));
        std::size_t consumed = 0;
        const int parsed = std::stoi(byte_text, &consumed, 16);
        if (consumed != 2) scenario_fail("invalid hex text");
        out.push_back(static_cast<std::uint8_t>(parsed));
    }
    return out;
}

// ---- sqlite executor (SqlExecutor boundary) -----------------------------------------------

class SqliteExecutor : public ledger::SqlExecutor {
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
        if (rc == SQLITE_ROW) {
            // java.sql.SQLException("Query returns results"): executeUpdate refuses a statement
            // that produces rows. The golden driver records it as a SQLException step result.
            sqlite3_finalize(statement);
            throw ledger::SqlError("Query returns results");
        }
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(statement);
        if ((rc & 0xFF) == SQLITE_CONSTRAINT) throw ledger::SqlConstraintError(message);
        throw ledger::SqlError("sqlite: " + message + " in " + sql);
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
            throw ledger::SqlError("sqlite: " + message + " in " + sql);
        }
        sqlite3_finalize(statement);
        return rows;
    }

    std::int64_t last_insert_rowid() override { return sqlite3_last_insert_rowid(db_); }

    void begin() override { exec_raw("BEGIN"); }
    void commit() override { exec_raw("COMMIT"); }
    void rollback() override { exec_raw("ROLLBACK"); }

private:
    sqlite3_stmt* prepare(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr);
        if (rc != SQLITE_OK) {
            throw ledger::SqlError(std::string("sqlite prepare: ") + sqlite3_errmsg(db_) + " in " +
                                   sql);
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
            if (rc != SQLITE_OK) {
                throw ledger::SqlError(std::string("sqlite bind: ") + sqlite3_errmsg(db_));
            }
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
                    const int size = sqlite3_column_bytes(statement, i);
                    value.text.assign(reinterpret_cast<const char*>(text),
                                      static_cast<std::size_t>(size));
                    break;
                }
                case SQLITE_BLOB: {
                    value.kind = SqlValue::Kind::Blob;
                    const auto* bytes = sqlite3_column_blob(statement, i);
                    const int size = sqlite3_column_bytes(statement, i);
                    const auto* first = static_cast<const std::uint8_t*>(bytes);
                    value.blob.assign(first, first + size);
                    break;
                }
                default:
                    throw ledger::SqlError("sqlite: unsupported column type");
            }
            row.push_back(std::move(value));
        }
        return row;
    }

    void exec_raw(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "statement failed";
            sqlite3_free(error);
            throw ledger::SqlError("sqlite: " + message);
        }
    }

    sqlite3* db_ = nullptr;
};

/** The ChatDao surface (see platform/StandChatDatabase.kt in the golden stand). */
class SqliteConversations : public ledger::ConversationSurface {
public:
    explicit SqliteConversations(SqliteExecutor& sql) : sql_(sql) {}

    std::optional<bool> conversation_is_task_free(std::string_view conversation_id) override {
        // ChatContextCompactDao.getConversation: SELECT * FROM conversations WHERE id = :conversationId
        std::vector<SqlRow> rows = sql_.query(
            "SELECT taskId FROM conversations WHERE id = ?",
            {SqlValue::text_value(std::string(conversation_id))});
        if (rows.empty()) return std::nullopt;
        return rows.front().front().is_null();
    }

    void delete_embeddings_by_conversation(std::string_view conversation_id) override {
        // ChatDao.deleteEmbeddingsByConversation (verbatim query text).
        sql_.execute(
            "DELETE FROM embeddings WHERE messageId IN "
            "(SELECT id FROM messages WHERE conversationId = ?)",
            {SqlValue::text_value(std::string(conversation_id))});
    }

private:
    SqliteExecutor& sql_;
};

// ---- scenario entities --------------------------------------------------------------------

LedgerEntry ledger_from_json(const json::Value& source) {
    LedgerEntry entry;
    entry.model_id = required_string(source, "modelId");
    entry.state = optional_string(source, "state").value_or("NEEDS_RECONCILE");
    entry.source_revision = optional_long(source, "sourceRevision").value_or(0);
    entry.completed_revision = optional_long(source, "completedRevision").value_or(0);
    entry.reconcile_revision = optional_long(source, "reconcileRevision").value_or(0);
    entry.updated_at = optional_long(source, "updatedAt").value_or(0);
    entry.validate();  // Kotlin's constructor runs the init block at this point
    return entry;
}

WorkEntry work_from_json(const json::Value& source) {
    WorkEntry entry;
    entry.model_id = required_string(source, "modelId");
    entry.message_id = required_string(source, "messageId");
    entry.source_fingerprint = optional_string(source, "sourceFingerprint");
    entry.source_revision = required_long(source, "sourceRevision");
    entry.updated_at = optional_long(source, "updatedAt").value_or(0);
    entry.validate();  // Kotlin's constructor runs the init block at this point
    return entry;
}

EmbeddingEntry embedding_from_json(const json::Value& source) {
    EmbeddingEntry entry;
    entry.id = optional_long(source, "id").value_or(0);
    entry.message_id = required_string(source, "messageId");
    entry.model_id = required_string(source, "modelId");
    entry.embedding = hex_to_bytes(required_string(source, "embedding"));
    entry.chunk_text = optional_string(source, "chunkText").value_or("");
    entry.dimension = optional_long(source, "dimension").value_or(0);
    entry.source_fingerprint = optional_string(source, "sourceFingerprint");
    return entry;
}

SemanticModelSnapshot snapshot_of(const json::Value& step) {
    // semanticModelSnapshot(activeModelId, configuredModelIds)
    return ledger::semantic_model_snapshot(required_string(step, "activeModelId"),
                                           string_array(step, "configured"));
}

// ---- rendering ----------------------------------------------------------------------------

json::Value ledger_json(const LedgerEntry& entry) {
    return json_object({{"modelId", json_string(entry.model_id)},
                        {"state", json_string(entry.state)},
                        {"sourceRevision", json_long(entry.source_revision)},
                        {"completedRevision", json_long(entry.completed_revision)},
                        {"reconcileRevision", json_long(entry.reconcile_revision)},
                        {"updatedAt", json_long(entry.updated_at)}});
}

json::Value work_json(const WorkEntry& entry) {
    return json_object({{"modelId", json_string(entry.model_id)},
                        {"messageId", json_string(entry.message_id)},
                        {"sourceFingerprint", optional_json_string(entry.source_fingerprint)},
                        {"sourceRevision", json_long(entry.source_revision)},
                        {"updatedAt", json_long(entry.updated_at)}});
}

json::Value reconcile_row_json(const ReconcileIndexableMessage& row) {
    json::Value dimension = row.dimension.has_value() ? json_long(*row.dimension) : json_null();
    json::Value bytes =
        row.embedding_bytes.has_value() ? json_long(*row.embedding_bytes) : json_null();
    json::Value embedding_id =
        row.embedding_id.has_value() ? json_long(*row.embedding_id) : json_null();
    return json_object({{"id", json_string(row.id)},
                        {"text", json_string(row.text)},
                        {"embeddingId", std::move(embedding_id)},
                        {"embeddingFingerprint", optional_json_string(row.embedding_fingerprint)},
                        {"dimension", std::move(dimension)},
                        {"embeddingBytes", std::move(bytes)}});
}

json::Value error_json(const ledger::KotlinError& failure) {
    return json_object({{"error", json_string(failure.class_name())},
                        {"message", json_string(failure.what())}});
}

// ---- driver --------------------------------------------------------------------------------

void run_sql(SqliteExecutor& sql, const json::Value& step) {
    SqlArgs args;
    if (const json::Value* array = field(step, "args"); array != nullptr) {
        if (array->kind != json::Kind::Array) scenario_fail("args must be an array");
        for (const json::Value& argument : array->elements) {
            if (is_null_element(argument)) {
                args.push_back(SqlValue::null());
            } else if (argument.kind == json::Kind::String) {
                args.push_back(SqlValue::text_value(argument.text));
            } else {
                const std::string& text = argument.text;
                const bool real = text.find('.') != std::string::npos ||
                                  text.find('e') != std::string::npos ||
                                  text.find('E') != std::string::npos;
                if (real) {
                    SqlValue value;
                    value.kind = SqlValue::Kind::Real;
                    value.real = std::stod(text);
                    args.push_back(value);
                } else {
                    args.push_back(SqlValue::integer_value(std::stoll(text)));
                }
            }
        }
    }
    sql.execute(required_string(step, "sql"), args);
}

void run_sql_block(SqliteExecutor& sql, const json::Value* block) {
    if (block == nullptr) return;
    if (block->kind != json::Kind::Array) scenario_fail("blockSql must be an array");
    for (const json::Value& statement : block->elements) run_sql(sql, statement);
}

json::Value dispatch(const std::string& op,
                     const json::Value& step,
                     ledger::SemanticIndexDao& dao,
                     SqliteConversations& conversations,
                     SqliteExecutor& sql) {
    if (op == "fingerprint") {
        return json_string(ledger::semantic_source_fingerprint(required_string(step, "text")));
    }
    if (op == "snapshot") {
        SemanticModelSnapshot snapshot = snapshot_of(step);
        std::vector<json::Value> configured;
        for (const std::string& model_id : snapshot.configured_model_ids) {
            configured.push_back(json_string(model_id));
        }
        return json_object({{"active", optional_json_string(snapshot.active_model_id)},
                            {"configured", json_array(std::move(configured))}});
    }
    if (op == "admitModel") {
        return ledger_json(dao.admit_model(required_string(step, "modelId"),
                                           required_long(step, "updatedAt")));
    }
    if (op == "requestReconcile") {
        return ledger_json(
            dao.request_reconcile(required_string(step, "modelId"), required_long(step, "updatedAt")));
    }
    if (op == "enqueueExactWork") {
        return ledger_json(dao.enqueue_exact_work(required_string(step, "modelId"),
                                                  required_string(step, "messageId"),
                                                  optional_string(step, "sourceFingerprint"),
                                                  required_long(step, "updatedAt")));
    }
    if (op == "completeExactWork") {
        const json::Value* work = field(step, "work");
        if (work == nullptr) scenario_fail("missing \"work\"");
        return json_bool(dao.complete_exact_work(work_from_json(*work),
                                                required_long(step, "updatedAt")));
    }
    if (op == "completeReconcile") {
        return json_bool(dao.complete_reconcile(required_string(step, "modelId"),
                                                required_long(step, "expectedReconcileRevision"),
                                                required_long(step, "updatedAt")));
    }
    if (op == "insertLedger") {
        const json::Value* ledger = field(step, "ledger");
        if (ledger == nullptr) scenario_fail("missing \"ledger\"");
        return json_long(dao.insert_ledger(ledger_from_json(*ledger)));
    }
    if (op == "getLedger") {
        std::optional<LedgerEntry> entry = dao.get_ledger(required_string(step, "modelId"));
        if (!entry.has_value()) return json_null();
        return ledger_json(*entry);
    }
    if (op == "getLedgers") {
        std::vector<json::Value> entries;
        for (const LedgerEntry& entry : dao.get_ledgers(string_array(step, "modelIds"))) {
            entries.push_back(ledger_json(entry));
        }
        return json_array(std::move(entries));
    }
    if (op == "advanceForExactWork") {
        return json_long(dao.advance_for_exact_work(required_string(step, "modelId"),
                                                    required_long(step, "updatedAt")));
    }
    if (op == "advanceForReconcile") {
        return json_long(dao.advance_for_reconcile(required_string(step, "modelId"),
                                                   required_long(step, "updatedAt")));
    }
    if (op == "upsertWork") {
        const json::Value* work = field(step, "work");
        if (work == nullptr) scenario_fail("missing \"work\"");
        dao.upsert_work(work_from_json(*work));
        return json_null();
    }
    if (op == "getWork") {
        std::optional<WorkEntry> entry =
            dao.get_work(required_string(step, "modelId"), required_string(step, "messageId"));
        if (!entry.has_value()) return json_null();
        return work_json(*entry);
    }
    if (op == "getWorkPage") {
        std::vector<json::Value> entries;
        for (const WorkEntry& entry : dao.get_work_page(
                 required_string(step, "modelId"), required_long(step, "maxSourceRevision"),
                 required_long(step, "afterSourceRevision"), required_string(step, "afterMessageId"),
                 static_cast<std::int32_t>(required_long(step, "limit")))) {
            entries.push_back(work_json(entry));
        }
        return json_array(std::move(entries));
    }
    if (op == "getWorkCountThroughRevision") {
        return json_long(dao.get_work_count_through_revision(
            required_string(step, "modelId"), required_long(step, "maxSourceRevision")));
    }
    if (op == "getReconcileMessagesPage") {
        std::vector<json::Value> rows;
        for (const ReconcileIndexableMessage& row : dao.get_reconcile_messages_page(
                 required_string(step, "modelId"), required_long(step, "maxWorkRevision"),
                 optional_string(step, "afterId"),
                 static_cast<std::int32_t>(required_long(step, "limit")))) {
            rows.push_back(reconcile_row_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "getSearchableMessageText") {
        return optional_json_string(
            dao.get_searchable_message_text(required_string(step, "messageId")));
    }
    if (op == "upsertEmbedding") {
        const json::Value* embedding = field(step, "embedding");
        if (embedding == nullptr) scenario_fail("missing \"embedding\"");
        dao.upsert_embedding(embedding_from_json(*embedding));
        return json_null();
    }
    if (op == "deleteEmbeddingsForMessages") {
        return json_long(
            dao.delete_embeddings_for_messages(string_array(step, "messageIds")));
    }
    if (op == "deleteEmbeddingsForModel") {
        return json_long(dao.delete_embeddings_for_model(required_string(step, "modelId")));
    }
    if (op == "deleteAllEmbeddings") {
        return json_long(dao.delete_all_embeddings());
    }
    if (op == "deleteWorkForModel") {
        return json_long(dao.delete_work_for_model(required_string(step, "modelId")));
    }
    if (op == "deleteMatchingWork") {
        return json_long(dao.delete_matching_work(required_string(step, "modelId"),
                                                  required_string(step, "messageId"),
                                                  optional_string(step, "sourceFingerprint"),
                                                  required_long(step, "sourceRevision")));
    }
    if (op == "markCurrentAfterExactWork") {
        return json_long(dao.mark_current_after_exact_work(required_string(step, "modelId"),
                                                           required_long(step, "sourceRevision"),
                                                           required_long(step, "updatedAt")));
    }
    if (op == "markAfterReconcile") {
        return json_long(dao.mark_after_reconcile(required_string(step, "modelId"),
                                                  required_long(step, "expectedReconcileRevision"),
                                                  required_long(step, "updatedAt")));
    }
    if (op == "deleteModel") {
        return json_long(dao.delete_model(required_string(step, "modelId")));
    }
    if (op == "requestSemanticReconcile") {
        ledger::request_semantic_reconcile(dao, snapshot_of(step), required_long(step, "updatedAt"));
        return json_null();
    }
    if (op == "withSemanticSourceMutation") {
        ledger::with_semantic_source_mutation(
            dao, snapshot_of(step), string_array(step, "messageIds"),
            required_long(step, "updatedAt"),
            [&] { run_sql_block(sql, field(step, "blockSql")); });
        return json_null();
    }
    if (op == "withSemanticGraphMutation") {
        ledger::with_semantic_graph_mutation(
            dao, snapshot_of(step), string_array(step, "clearMessageIds"),
            optional_boolean(step, "clearAllEmbeddings", false), required_long(step, "updatedAt"),
            [&] { run_sql_block(sql, field(step, "blockSql")); });
        return json_null();
    }
    if (op == "withSemanticEligibilityMutation") {
        ledger::with_semantic_eligibility_mutation(
            dao, conversations, snapshot_of(step), required_string(step, "conversationId"),
            required_long(step, "updatedAt"),
            [&] { run_sql_block(sql, field(step, "blockSql")); });
        return json_null();
    }
    if (op == "commitSemanticEmbedding") {
        const json::Value* embedding = field(step, "embedding");
        if (embedding == nullptr) scenario_fail("missing \"embedding\"");
        return json_bool(ledger::commit_semantic_embedding(
            dao, embedding_from_json(*embedding), required_string(step, "expectedFingerprint"),
            required_long(step, "updatedAt"), optional_long(step, "expectedWorkRevision"),
            optional_long(step, "expectedReconcileRevision"),
            optional_boolean(step, "completePendingWork", true)));
    }
    if (op == "invalidateSemanticModel") {
        ledger::invalidate_semantic_model(dao, required_string(step, "modelId"),
                                          required_long(step, "updatedAt"));
        return json_null();
    }
    if (op == "deleteSemanticModel") {
        ledger::delete_semantic_model(dao, required_string(step, "modelId"));
        return json_null();
    }
    if (op == "sql") {
        SqlArgs args;
        if (const json::Value* array = field(step, "args"); array != nullptr) {
            for (const json::Value& argument : array->elements) {
                if (is_null_element(argument)) {
                    args.push_back(SqlValue::null());
                } else if (argument.kind == json::Kind::String) {
                    args.push_back(SqlValue::text_value(argument.text));
                } else {
                    args.push_back(SqlValue::integer_value(std::stoll(argument.text)));
                }
            }
        }
        return json_long(sql.execute(required_string(step, "sql"), args));
    }
    throw ledger::KotlinError("IllegalStateException", "unknown operation " + op);
}

void seed(SqliteExecutor& sql, const json::Value& root) {
    const json::Value* seed_object = field(root, "seed");
    if (seed_object == nullptr) return;
    if (const json::Value* conversations = field(*seed_object, "conversations");
        conversations != nullptr) {
        for (const json::Value& row : conversations->elements) {
            sql.execute("INSERT INTO conversations (id, taskId) VALUES (?, ?)",
                        {SqlValue::text_value(required_string(row, "id")),
                         optional_string(row, "taskId").has_value()
                             ? SqlValue::text_value(*optional_string(row, "taskId"))
                             : SqlValue::null()});
        }
    }
    if (const json::Value* messages = field(*seed_object, "messages"); messages != nullptr) {
        for (const json::Value& row : messages->elements) {
            sql.execute("INSERT INTO messages (id, conversationId, text, participant) VALUES (?, ?, ?, ?)",
                        {SqlValue::text_value(required_string(row, "id")),
                         SqlValue::text_value(required_string(row, "conversationId")),
                         SqlValue::text_value(required_string(row, "text")),
                         SqlValue::text_value(required_string(row, "participant"))});
        }
    }
    if (const json::Value* embeddings = field(*seed_object, "embeddings");
        embeddings != nullptr) {
        for (const json::Value& row : embeddings->elements) {
            SqlValue blob;
            blob.kind = SqlValue::Kind::Blob;
            blob.blob = hex_to_bytes(required_string(row, "embedding"));
            sql.execute(
                "INSERT INTO embeddings (id, messageId, modelId, embedding, chunkText, dimension, "
                "sourceFingerprint) VALUES (?, ?, ?, ?, ?, ?, ?)",
                {optional_long(row, "id").has_value()
                     ? SqlValue::integer_value(*optional_long(row, "id"))
                     : SqlValue::null(),
                 SqlValue::text_value(required_string(row, "messageId")),
                 SqlValue::text_value(required_string(row, "modelId")), std::move(blob),
                 SqlValue::text_value(optional_string(row, "chunkText").value_or("")),
                 SqlValue::integer_value(optional_long(row, "dimension").value_or(0)),
                 optional_string(row, "sourceFingerprint").has_value()
                     ? SqlValue::text_value(*optional_string(row, "sourceFingerprint"))
                     : SqlValue::null()});
        }
    }
}

json::Value query_rows(SqliteExecutor& sql, const std::string& query,
                       json::Value (*render)(const SqlRow&)) {
    std::vector<json::Value> rows;
    for (const SqlRow& row : sql.query(query, {})) rows.push_back(render(row));
    return json_array(std::move(rows));
}

json::Value ledger_row_json(const SqlRow& row) {
    return json_object({{"modelId", json_string(row[0].as_text())},
                        {"state", json_string(row[1].as_text())},
                        {"sourceRevision", json_long(row[2].as_integer())},
                        {"completedRevision", json_long(row[3].as_integer())},
                        {"reconcileRevision", json_long(row[4].as_integer())},
                        {"updatedAt", json_long(row[5].as_integer())}});
}

json::Value work_row_json(const SqlRow& row) {
    return json_object({{"modelId", json_string(row[0].as_text())},
                        {"messageId", json_string(row[1].as_text())},
                        {"sourceFingerprint", optional_json_string(row[2].text_or_null())},
                        {"sourceRevision", json_long(row[3].as_integer())},
                        {"updatedAt", json_long(row[4].as_integer())}});
}

json::Value embedding_row_json(const SqlRow& row) {
    return json_object({{"id", json_long(row[0].as_integer())},
                        {"messageId", json_string(row[1].as_text())},
                        {"modelId", json_string(row[2].as_text())},
                        {"embedding", json_string(hex_lower(row[3].as_blob()))},
                        {"chunkText", json_string(row[4].as_text())},
                        {"dimension", json_long(row[5].as_integer())},
                        {"sourceFingerprint", optional_json_string(row[6].text_or_null())}});
}

json::Value message_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"conversationId", json_string(row[1].as_text())},
                        {"text", json_string(row[2].as_text())},
                        {"participant", json_string(row[3].as_text())}});
}

json::Value conversation_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"taskId", optional_json_string(row[1].text_or_null())}});
}

json::Value dump_tables(SqliteExecutor& sql) {
    return json_object(
        {{"semantic_index_ledger",
          query_rows(sql,
                     "SELECT modelId, state, sourceRevision, completedRevision, reconcileRevision, "
                     "updatedAt FROM semantic_index_ledger ORDER BY modelId",
                     ledger_row_json)},
         {"semantic_index_work",
          query_rows(sql,
                     "SELECT modelId, messageId, sourceFingerprint, sourceRevision, updatedAt "
                     "FROM semantic_index_work ORDER BY modelId, messageId",
                     work_row_json)},
         {"embeddings",
          query_rows(sql,
                     "SELECT id, messageId, modelId, embedding, chunkText, dimension, "
                     "sourceFingerprint FROM embeddings ORDER BY id",
                     embedding_row_json)},
         {"messages",
          query_rows(sql, "SELECT id, conversationId, text, participant FROM messages ORDER BY id",
                     message_row_json)},
         {"conversations",
          query_rows(sql, "SELECT id, taskId FROM conversations ORDER BY id",
                     conversation_row_json)}});
}

std::string run_scenario(const std::filesystem::path& scenario) {
    const std::filesystem::path root_dir = AGORA_FIXTURE_SEMANTIC_INDEX_LEDGER_DIR;
    const json::ParseResult parsed = json::parse(read_file(scenario));
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        scenario_fail(scenario.stem().string() + ": scenario must be an object");
    }
    const json::Value& root = *parsed.value;

    SqliteExecutor sql(root_dir / "platform" / "schema.sql");
    seed(sql, root);
    ledger::SemanticIndexDao dao(sql);
    SqliteConversations conversations(sql);

    std::vector<json::Value> steps;
    const json::Value* step_list = field(root, "steps");
    if (step_list != nullptr && step_list->kind == json::Kind::Array) {
        for (const json::Value& element : step_list->elements) {
            const json::Value& step = element;
            const std::string op = required_string(step, "op");
            json::Value result;
            try {
                result = dispatch(op, step, dao, conversations, sql);
            } catch (const ledger::KotlinError& failure) {
                result = error_json(failure);
            } catch (const ledger::SqlError& failure) {
                // The Kotlin driver executes blockSql/sql outside the DAO proxy, so JDBC failures
                // surface as java.sql.SQLException instead of the proxy's UndeclaredThrowable wrapper.
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

TEST(semantic_index_ledger_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SEMANTIC_INDEX_LEDGER_DIR;
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
