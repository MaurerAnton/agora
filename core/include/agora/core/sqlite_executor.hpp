#pragma once

#include "agora/core/sql_boundary.hpp"

#include <memory>
#include <sqlite3.h>
#include <string>

namespace agora::core {

/** Concrete SqlExecutor backed by sqlite3. */
class SqliteExecutor final : public SqlExecutor {
public:
    explicit SqliteExecutor(const std::string& db_path);
    ~SqliteExecutor() override;

    // Non-copyable, movable
    SqliteExecutor(const SqliteExecutor&) = delete;
    SqliteExecutor& operator=(const SqliteExecutor&) = delete;
    SqliteExecutor(SqliteExecutor&&) noexcept;
    SqliteExecutor& operator=(SqliteExecutor&&) noexcept;

    std::int64_t execute(const std::string& sql, const SqlArgs& args) override;
    std::vector<SqlRow> query(const std::string& sql, const SqlArgs& args) override;
    std::int64_t last_insert_rowid() override;
    void begin() override;
    void commit() override;
    void rollback() override;

    /** Returns the raw sqlite3* for schema initialization / migrations. */
    sqlite3* handle() const { return db_; }

private:
    sqlite3* db_ = nullptr;

    void bind_args(sqlite3_stmt* stmt, const SqlArgs& args);
    std::vector<SqlRow> fetch_all(sqlite3_stmt* stmt);
};

}  // namespace agora::core