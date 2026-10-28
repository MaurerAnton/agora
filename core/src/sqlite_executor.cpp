#include "agora/core/sqlite_executor.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace agora::core {

namespace {

class SqliteError : public std::runtime_error {
public:
    explicit SqliteError(const std::string& message) : std::runtime_error(message) {}
};

void check_sqlite(int rc, sqlite3* db, const char* context) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw SqliteError(std::string(context) + ": " + sqlite3_errmsg(db));
    }
}

}  // namespace

SqliteExecutor::SqliteExecutor(const std::string& db_path) {
    int rc = sqlite3_open_v2(db_path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError("sqlite3_open_v2: " + std::string(sqlite3_errmsg(db_)));
    }
    // Enable foreign keys
    rc = sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError("PRAGMA foreign_keys: " + std::string(sqlite3_errmsg(db_)));
    }
    // Use WAL mode for better concurrency
    rc = sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError("PRAGMA journal_mode: " + std::string(sqlite3_errmsg(db_)));
    }
}

SqliteExecutor::~SqliteExecutor() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

SqliteExecutor::SqliteExecutor(SqliteExecutor&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

SqliteExecutor& SqliteExecutor::operator=(SqliteExecutor&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close_v2(db_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void SqliteExecutor::bind_args(sqlite3_stmt* stmt, const SqlArgs& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const SqlValue& val = args[i];
        int rc = SQLITE_OK;
        switch (val.kind) {
            case SqlValue::Kind::Null:
                rc = sqlite3_bind_null(stmt, static_cast<int>(i + 1));
                break;
            case SqlValue::Kind::Text:
                rc = sqlite3_bind_text(stmt, static_cast<int>(i + 1), val.text.c_str(),
                                       static_cast<int>(val.text.size()), SQLITE_TRANSIENT);
                break;
            case SqlValue::Kind::Integer:
                rc = sqlite3_bind_int64(stmt, static_cast<int>(i + 1), val.integer);
                break;
            case SqlValue::Kind::Real: {
                double real_val = val.real;
                rc = sqlite3_bind_double(stmt, static_cast<int>(i + 1), real_val);
                break;
            }
            case SqlValue::Kind::Blob: {
                rc = sqlite3_bind_blob(stmt, static_cast<int>(i + 1), val.blob.data(),
                                       static_cast<int>(val.blob.size()), SQLITE_TRANSIENT);
                break;
            }
        }
        if (rc != SQLITE_OK) {
            throw SqliteError("sqlite3_bind: " + std::string(sqlite3_errmsg(db_)));
        }
    }
}

std::vector<SqlRow> SqliteExecutor::fetch_all(sqlite3_stmt* stmt) {
    std::vector<SqlRow> rows;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        SqlRow row;
        int cols = sqlite3_column_count(stmt);
        row.reserve(cols);
        for (int i = 0; i < cols; ++i) {
            int type = sqlite3_column_type(stmt, i);
            switch (type) {
                case SQLITE_NULL:
                    row.push_back(SqlValue::null());
                    break;
                case SQLITE_TEXT: {
                    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                    int len = sqlite3_column_bytes(stmt, i);
                    row.push_back(SqlValue::text_value(std::string(text, len)));
                    break;
                }
                case SQLITE_INTEGER: {
                    long long val = sqlite3_column_int64(stmt, i);
                    row.push_back(SqlValue::integer_value(val));
                    break;
                }
                case SQLITE_FLOAT: {
                    double val = sqlite3_column_double(stmt, i);
                    row.push_back(SqlValue::integer_value(static_cast<long long>(val)));
                    break;
                }
                case SQLITE_BLOB: {
                    const void* blob = sqlite3_column_blob(stmt, i);
                    int len = sqlite3_column_bytes(stmt, i);
                    row.push_back(SqlValue::text_value(std::string(
                        static_cast<const char*>(blob), len)));
                    break;
                }
            }
        }
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) {
        throw SqliteError("sqlite3_step: " + std::string(sqlite3_errmsg(db_)));
    }
    return rows;
}

std::int64_t SqliteExecutor::execute(const std::string& sql, const SqlArgs& args) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(db_)) + " SQL: " + sql);
    }
    bind_args(stmt, args);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw SqliteError("sqlite3_step: " + std::string(sqlite3_errmsg(db_)));
    }
    std::int64_t changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return changes;
}

std::vector<SqlRow> SqliteExecutor::query(const std::string& sql, const SqlArgs& args) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(db_)) + " SQL: " + sql);
    }
    bind_args(stmt, args);
    auto rows = fetch_all(stmt);
    sqlite3_finalize(stmt);
    return rows;
}

std::int64_t SqliteExecutor::last_insert_rowid() {
    return sqlite3_last_insert_rowid(db_);
}

void SqliteExecutor::begin() {
    int rc = sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError("BEGIN: " + std::string(sqlite3_errmsg(db_)));
    }
}

void SqliteExecutor::commit() {
    int rc = sqlite3_exec(db_, "COMMIT TRANSACTION;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError("COMMIT: " + std::string(sqlite3_errmsg(db_)));
    }
}

void SqliteExecutor::rollback() {
    int rc = sqlite3_exec(db_, "ROLLBACK TRANSACTION;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        throw SqliteError("ROLLBACK: " + std::string(sqlite3_errmsg(db_)));
    }
}

}  // namespace agora::core