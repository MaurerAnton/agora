#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace agora::core::sqlite_platform {

// Shared platform surface for the Room migration ports. Kotlin's SupportSQLiteDatabase and
// android.database.Cursor are platform types; the ports ask this interface for the same typed
// projections the app migrations read, while the SQL text and bind arguments stay application
// logic (and are pinned by the golden stands).

/** A bind argument with the Kotlin runtime type preserved (SQL arg arrays box Any?). */
struct SqlValue {
    enum class Kind {
        Null,
        Text,
        Long,
        Int,
    };

    Kind kind = Kind::Null;
    std::string text;
    long long number = 0;

    static SqlValue null();
    static SqlValue text_value(std::string value);
    static SqlValue long_value(long long value);
    static SqlValue int_value(int value);
};

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<SqlValue>> rows;

    /** androidx Cursor.getColumnIndexOrThrow: IllegalArgumentException when absent. */
    int column_index(std::string_view name) const;
};

/** The androidx.sqlite database surface the migrations use; implemented by the test shims. */
class Database {
public:
    virtual ~Database() = default;
    virtual void exec_sql(std::string_view sql, const std::vector<SqlValue>& args) = 0;
    virtual QueryResult query(std::string_view sql, const std::vector<SqlValue>& args) = 0;
};

/** Kotlin require/check failures are surfaced with the JVM exception class the driver prints. */
struct MigrationException {
    std::string class_name;
    std::string message;
};

}  // namespace agora::core::sqlite_platform
