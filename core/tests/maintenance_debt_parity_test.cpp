// Golden parity test for the maintenance debt queue (data/local/MaintenanceDebt.kt).
//
// The stand runs the Kotlin DAO against a real SQLite database through Room's replaced annotation
// processor; this test runs the C++ port against the same schema (platform/schema.sql, shipped
// with the stand) through a sqlite3 implementation of SqlExecutor. Each step records its result
// (or the Kotlin validation error it raises) and the run ends with a dump of the five observed
// tables. Wall-clock values written by repairRunBranches are masked identically on both sides.
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
#include "agora/core/maintenance_debt.hpp"
#include "check.hpp"

namespace {

namespace debt = agora::core::maintenance_debt;
namespace json = agora::core::json;

using agora::core::KotlinError;
using agora::core::SqlArgs;
using agora::core::SqlError;
using agora::core::SqlRow;
using agora::core::SqlValue;
using debt::ConversationEntry;
using debt::DebtEntry;
using debt::RunEntry;

/**
 * System.currentTimeMillis() inside repairRunBranches: the port receives it as a parameter, so
 * the test passes a fixed value inside the mask window the stand uses for real clock writes.
 */
constexpr std::int64_t kNow = 1'700'000'000'000;
constexpr std::int64_t kClockLow = 1'000'000'000'000;
constexpr std::int64_t kClockHigh = 4'000'000'000'000;

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

std::int32_t required_int(const json::Value& object, std::string_view name) {
    return static_cast<std::int32_t>(required_long(object, name));
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

json::Value optional_json_long(const std::optional<std::int64_t>& number) {
    if (!number.has_value()) return json_null();
    return json_long(*number);
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
        if (rc == SQLITE_ROW) {
            // java.sql.SQLException("Query returns results"): executeUpdate refuses a statement
            // that produces rows. The golden driver records it as a SQLException step result.
            sqlite3_finalize(statement);
            throw SqlError("Query returns results");
        }
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(statement);
        if ((rc & 0xFF) == SQLITE_CONSTRAINT) throw agora::core::SqlConstraintError(message);
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
                    throw SqlError("sqlite: unsupported column type");
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
            throw SqlError("sqlite: " + message);
        }
    }

    sqlite3* db_ = nullptr;
};

// ---- scenario entities --------------------------------------------------------------------

DebtEntry debt_from_json(const json::Value& source) {
    DebtEntry entry;
    entry.kind = required_string(source, "kind");
    entry.identity = required_string(source, "identity");
    entry.state = optional_string(source, "state").value_or("PENDING");
    entry.revision = optional_long(source, "revision").value_or(1);
    entry.updated_at = optional_long(source, "updatedAt").value_or(0);
    entry.claim_id = optional_string(source, "claimId");
    entry.claimed_at = optional_long(source, "claimedAt");
    entry.validate();  // Kotlin's constructor runs the init block at this point
    return entry;
}

// ---- rendering ----------------------------------------------------------------------------

json::Value debt_json(const DebtEntry& entry) {
    return json_object({{"kind", json_string(entry.kind)},
                        {"identity", json_string(entry.identity)},
                        {"state", json_string(entry.state)},
                        {"revision", json_long(entry.revision)},
                        {"updatedAt", json_long(entry.updated_at)},
                        {"claimId", optional_json_string(entry.claim_id)},
                        {"claimedAt", optional_json_long(entry.claimed_at)}});
}

json::Value run_json(const RunEntry& entry) {
    return json_object({{"id", json_string(entry.id)},
                        {"conversationId", json_string(entry.conversation_id)},
                        {"parentRunId", optional_json_string(entry.parent_run_id)},
                        {"status", json_string(entry.status)},
                        {"activeSlot", optional_json_long(entry.active_slot)},
                        {"startedAt", json_long(entry.started_at)},
                        {"lastCheckpointAt", json_long(entry.last_checkpoint_at)},
                        {"stopRequestedAt", optional_json_long(entry.stop_requested_at)},
                        {"endedAt", optional_json_long(entry.ended_at)},
                        {"endReason", optional_json_string(entry.end_reason)},
                        {"currentPass", json_long(entry.current_pass)},
                        {"legacyAmbiguous", json_bool(entry.legacy_ambiguous)}});
}

json::Value conversation_json(const ConversationEntry& entry) {
    return json_object({{"id", json_string(entry.id)},
                        {"selectedRunBranchesJson",
                         optional_json_string(entry.selected_run_branches_json)}});
}

json::Value error_json(const KotlinError& failure) {
    return json_object({{"error", json_string(failure.class_name())},
                        {"message", json_string(failure.what())}});
}

json::Value debt_array(const std::vector<DebtEntry>& entries) {
    std::vector<json::Value> out;
    for (const DebtEntry& entry : entries) out.push_back(debt_json(entry));
    return json_array(std::move(out));
}

json::Value long_array(const std::vector<std::int64_t>& values) {
    std::vector<json::Value> out;
    for (std::int64_t value : values) out.push_back(json_long(value));
    return json_array(std::move(out));
}

json::Value string_array(const std::vector<std::string>& values) {
    std::vector<json::Value> out;
    for (const std::string& value : values) out.push_back(json_string(value));
    return json_array(std::move(out));
}

// ---- driver --------------------------------------------------------------------------------

std::int64_t run_sql(SqliteExecutor& sql, const json::Value& step) {
    SqlArgs args;
    if (const json::Value* array = field(step, "args"); array != nullptr) {
        if (array->kind != json::Kind::Array) scenario_fail("args must be an array");
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
    return sql.execute(required_string(step, "sql"), args);
}

json::Value dispatch(const std::string& op, const json::Value& step, debt::MaintenanceDebtDao& dao,
                     SqliteExecutor& sql) {
    if (op == "insertDebt") {
        const json::Value* source = field(step, "debt");
        if (source == nullptr) scenario_fail("missing \"debt\"");
        return json_long(dao.insert_debt(debt_from_json(*source)));
    }
    if (op == "renewDebt") {
        return json_long(dao.renew_debt(required_string(step, "kind"),
                                        required_string(step, "identity"),
                                        required_long(step, "updatedAt")));
    }
    if (op == "getDebt") {
        std::optional<DebtEntry> entry =
            dao.get_debt(required_string(step, "kind"), required_string(step, "identity"));
        if (!entry.has_value()) return json_null();
        return debt_json(*entry);
    }
    if (op == "getClaimCandidates") {
        return debt_array(
            dao.get_claim_candidates(required_long(step, "staleBefore"), required_int(step, "limit")));
    }
    if (op == "claimCandidate") {
        return json_long(dao.claim_candidate(required_string(step, "kind"),
                                             required_string(step, "identity"),
                                             required_long(step, "revision"),
                                             required_string(step, "claimId"),
                                             required_long(step, "claimedAt"),
                                             required_long(step, "staleBefore")));
    }
    if (op == "hasDebt") {
        return json_bool(dao.has_debt());
    }
    if (op == "getOrphanEmbeddingIdsPage") {
        return long_array(dao.get_orphan_embedding_ids_page(required_long(step, "afterId"),
                                                            required_int(step, "limit")));
    }
    if (op == "deleteOrphanEmbeddingsForMessage") {
        return json_long(dao.delete_orphan_embeddings_for_message(required_string(step, "messageId")));
    }
    if (op == "deleteOrphanEmbeddingsByIds") {
        const json::Value* array = field(step, "ids");
        if (array == nullptr || array->kind != json::Kind::Array) {
            scenario_fail("ids must be an array");
        }
        std::vector<std::int64_t> ids;
        for (const json::Value& element : array->elements) {
            ids.push_back(std::stoll(primitive_content(element)));
        }
        return json_long(dao.delete_orphan_embeddings_by_ids(ids));
    }
    if (op == "getRunBranchConversationIdsPage") {
        return string_array(dao.get_run_branch_conversation_ids_page(
            optional_string(step, "afterId"), required_int(step, "limit")));
    }
    if (op == "getMaintenanceConversation") {
        std::optional<ConversationEntry> entry =
            dao.get_maintenance_conversation(required_string(step, "conversationId"));
        if (!entry.has_value()) return json_null();
        return conversation_json(*entry);
    }
    if (op == "getMaintenanceRuns") {
        std::vector<json::Value> runs;
        for (const RunEntry& run : dao.get_maintenance_runs(required_string(step, "conversationId"))) {
            runs.push_back(run_json(run));
        }
        return json_array(std::move(runs));
    }
    if (op == "compareAndSetMaintenanceRunBranches") {
        return json_long(dao.compare_and_set_maintenance_run_branches(
            required_string(step, "conversationId"), required_string(step, "expected"),
            required_string(step, "replacement"), required_long(step, "at")));
    }
    if (op == "completeClaimed") {
        return json_long(dao.complete_claimed(required_string(step, "kind"),
                                              required_string(step, "identity"),
                                              required_long(step, "revision"),
                                              required_string(step, "claimId")));
    }
    if (op == "releaseClaimed") {
        return json_long(dao.release_claimed(required_string(step, "kind"),
                                             required_string(step, "identity"),
                                             required_long(step, "revision"),
                                             required_string(step, "claimId"),
                                             required_long(step, "updatedAt")));
    }
    if (op == "enqueue") {
        return debt_json(
            dao.enqueue(required_string(step, "kind"), required_string(step, "identity"),
                        required_long(step, "updatedAt")));
    }
    if (op == "claim") {
        return debt_array(dao.claim(required_string(step, "claimId"), required_long(step, "claimedAt"),
                                    required_long(step, "staleBefore"),
                                    required_int(step, "limit")));
    }
    if (op == "repairRunBranches") {
        return json_bool(dao.repair_run_branches(required_string(step, "conversationId"), kNow));
    }
    if (op == "complete") {
        const json::Value* source = field(step, "debt");
        if (source == nullptr) scenario_fail("missing \"debt\"");
        return json_bool(dao.complete(debt_from_json(*source)));
    }
    if (op == "release") {
        const json::Value* source = field(step, "debt");
        if (source == nullptr) scenario_fail("missing \"debt\"");
        return json_bool(dao.release(debt_from_json(*source), required_long(step, "updatedAt")));
    }
    if (op == "sql") {
        // The Kotlin driver reports executeUpdate's changed-row count for the sql op.
        return json_long(run_sql(sql, step));
    }
    throw KotlinError("IllegalStateException", "unknown operation " + op);
}

void seed(SqliteExecutor& sql, const json::Value& root) {
    const json::Value* seed_object = field(root, "seed");
    if (seed_object == nullptr) return;
    if (const json::Value* conversations = field(*seed_object, "conversations");
        conversations != nullptr) {
        for (const json::Value& row : conversations->elements) {
            sql.execute(
                "INSERT INTO conversations (id, selectedRunBranchesJson, dataChangedAt) "
                "VALUES (?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 optional_string(row, "selectedRunBranchesJson").has_value()
                     ? SqlValue::text_value(*optional_string(row, "selectedRunBranchesJson"))
                     : SqlValue::null(),
                 SqlValue::integer_value(optional_long(row, "dataChangedAt").value_or(0))});
        }
    }
    if (const json::Value* runs = field(*seed_object, "runs"); runs != nullptr) {
        for (const json::Value& row : runs->elements) {
            const std::int64_t started_at = required_long(row, "startedAt");
            SqlArgs args;
            args.push_back(SqlValue::text_value(required_string(row, "id")));
            args.push_back(SqlValue::text_value(required_string(row, "conversationId")));
            const std::optional<std::string> parent = optional_string(row, "parentRunId");
            args.push_back(parent.has_value() ? SqlValue::text_value(*parent) : SqlValue::null());
            args.push_back(SqlValue::text_value(required_string(row, "status")));
            const std::optional<std::int64_t> slot = optional_long(row, "activeSlot");
            args.push_back(slot.has_value() ? SqlValue::integer_value(*slot) : SqlValue::null());
            args.push_back(SqlValue::integer_value(started_at));
            args.push_back(SqlValue::integer_value(optional_long(row, "lastCheckpointAt").value_or(started_at)));
            const std::optional<std::int64_t> stopped = optional_long(row, "stopRequestedAt");
            args.push_back(stopped.has_value() ? SqlValue::integer_value(*stopped) : SqlValue::null());
            const std::optional<std::int64_t> ended = optional_long(row, "endedAt");
            args.push_back(ended.has_value() ? SqlValue::integer_value(*ended) : SqlValue::null());
            const std::optional<std::string> reason = optional_string(row, "endReason");
            args.push_back(reason.has_value() ? SqlValue::text_value(*reason) : SqlValue::null());
            args.push_back(SqlValue::integer_value(optional_long(row, "currentPass").value_or(0)));
            const std::optional<std::string> ambiguous = optional_string(row, "legacyAmbiguous");
            args.push_back(SqlValue::integer_value(
                ambiguous.has_value() && *ambiguous == "true" ? 1 : 0));
            sql.execute(
                "INSERT INTO runs (id, conversationId, parentRunId, status, activeSlot, startedAt, "
                "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, "
                "legacyAmbiguous) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                args);
        }
    }
    if (const json::Value* messages = field(*seed_object, "messages"); messages != nullptr) {
        for (const json::Value& row : messages->elements) {
            sql.execute(
                "INSERT INTO messages (id, conversationId, text, participant) VALUES (?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 SqlValue::text_value(required_string(row, "conversationId")),
                 SqlValue::text_value(optional_string(row, "text").value_or("")),
                 SqlValue::text_value(optional_string(row, "participant").value_or("USER"))});
        }
    }
    if (const json::Value* embeddings = field(*seed_object, "embeddings");
        embeddings != nullptr) {
        for (const json::Value& row : embeddings->elements) {
            SqlValue blob;
            blob.kind = SqlValue::Kind::Blob;
            blob.blob = hex_to_bytes(required_string(row, "embedding"));
            const std::optional<std::int64_t> id = optional_long(row, "id");
            const std::optional<std::string> fingerprint = optional_string(row, "sourceFingerprint");
            sql.execute(
                "INSERT INTO embeddings (id, messageId, modelId, embedding, chunkText, dimension, "
                "sourceFingerprint) VALUES (?, ?, ?, ?, ?, ?, ?)",
                {id.has_value() ? SqlValue::integer_value(*id) : SqlValue::null(),
                 SqlValue::text_value(required_string(row, "messageId")),
                 SqlValue::text_value(required_string(row, "modelId")), std::move(blob),
                 SqlValue::text_value(optional_string(row, "chunkText").value_or("")),
                 SqlValue::integer_value(optional_long(row, "dimension").value_or(0)),
                 fingerprint.has_value() ? SqlValue::text_value(*fingerprint) : SqlValue::null()});
        }
    }
}

// ---- table dump ----------------------------------------------------------------------------

json::Value query_rows(SqliteExecutor& sql, const std::string& query,
                       json::Value (*render)(const SqlRow&)) {
    std::vector<json::Value> rows;
    for (const SqlRow& row : sql.query(query, {})) rows.push_back(render(row));
    return json_array(std::move(rows));
}

json::Value debt_row_json(const SqlRow& row) {
    return json_object({{"kind", json_string(row[0].as_text())},
                        {"identity", json_string(row[1].as_text())},
                        {"state", json_string(row[2].as_text())},
                        {"revision", json_long(row[3].as_integer())},
                        {"updatedAt", json_long(row[4].as_integer())},
                        {"claimId", optional_json_string(row[5].text_or_null())},
                        {"claimedAt", optional_json_long(row[6].integer_or_null())}});
}

/** Values the repair's wall-clock wrote are masked; anything else prints verbatim. */
json::Value clock_masked(std::int64_t value) {
    if (value >= kClockLow && value < kClockHigh) return json_string("<clock>");
    return json_long(value);
}

json::Value conversation_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"selectedRunBranchesJson", optional_json_string(row[1].text_or_null())},
                        {"dataChangedAt", clock_masked(row[2].as_integer())}});
}

json::Value run_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"conversationId", json_string(row[1].as_text())},
                        {"parentRunId", optional_json_string(row[2].text_or_null())},
                        {"status", json_string(row[3].as_text())},
                        {"activeSlot", optional_json_long(row[4].integer_or_null())},
                        {"startedAt", json_long(row[5].as_integer())},
                        {"lastCheckpointAt", json_long(row[6].as_integer())},
                        {"stopRequestedAt", optional_json_long(row[7].integer_or_null())},
                        {"endedAt", optional_json_long(row[8].integer_or_null())},
                        {"endReason", optional_json_string(row[9].text_or_null())},
                        {"currentPass", json_long(row[10].as_integer())},
                        {"legacyAmbiguous", json_bool(row[11].as_integer() != 0)}});
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

json::Value dump_tables(SqliteExecutor& sql) {
    return json_object(
        {{"maintenance_debt",
          query_rows(sql,
                     "SELECT kind, identity, state, revision, updatedAt, claimId, claimedAt "
                     "FROM maintenance_debt ORDER BY kind, identity",
                     debt_row_json)},
         {"conversations",
          query_rows(sql,
                     "SELECT id, selectedRunBranchesJson, dataChangedAt FROM conversations "
                     "ORDER BY id",
                     conversation_row_json)},
         {"runs",
          query_rows(sql,
                     "SELECT id, conversationId, parentRunId, status, activeSlot, startedAt, "
                     "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, "
                     "legacyAmbiguous FROM runs ORDER BY id",
                     run_row_json)},
         {"embeddings",
          query_rows(sql,
                     "SELECT id, messageId, modelId, embedding, chunkText, dimension, "
                     "sourceFingerprint FROM embeddings ORDER BY id",
                     embedding_row_json)},
         {"messages",
          query_rows(sql, "SELECT id, conversationId, text, participant FROM messages ORDER BY id",
                     message_row_json)}});
}

std::string run_scenario(const std::filesystem::path& scenario) {
    const std::filesystem::path root_dir = AGORA_FIXTURE_MAINTENANCE_DEBT_DIR;
    const json::ParseResult parsed = json::parse(read_file(scenario));
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        scenario_fail(scenario.stem().string() + ": scenario must be an object");
    }
    const json::Value& root = *parsed.value;

    SqliteExecutor sql(root_dir / "platform" / "schema.sql");
    seed(sql, root);
    debt::MaintenanceDebtDao dao(sql);

    std::vector<json::Value> steps;
    const json::Value* step_list = field(root, "steps");
    if (step_list != nullptr && step_list->kind == json::Kind::Array) {
        for (const json::Value& element : step_list->elements) {
            const std::string op = required_string(element, "op");
            json::Value result;
            try {
                result = dispatch(op, element, dao, sql);
            } catch (const KotlinError& failure) {
                result = error_json(failure);
            } catch (const SqlError& failure) {
                // The Kotlin driver executes sql ops outside the DAO proxy, so JDBC failures
                // surface as java.sql.SQLException instead of the proxy's wrapper.
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

TEST(maintenance_debt_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MAINTENANCE_DEBT_DIR;
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
