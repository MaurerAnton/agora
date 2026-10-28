// Smoke test for the SqliteExecutor implementation.

#include "agora/core/sqlite_executor.hpp"

#include <filesystem>
#include <string>

#include "check.hpp"

using agora::core::SqliteExecutor;
using agora::core::SqlExecutor;
using agora::core::SqlArgs;
using agora::core::SqlValue;
using agora::core::SqlRow;

namespace {

TEST(sqlite_executor_basic_operations) {
    // Create an in-memory database
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path db_path = temp_dir / "agora_sqlite_test.db";
    std::error_code ec;
    std::filesystem::remove(db_path, ec);

    SqliteExecutor exec(db_path.string());

    // Create a table
    exec.execute("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT, value INTEGER)", {});

    // Insert rows
    SqlArgs args1 = {SqlValue::integer_value(1), SqlValue::text_value("first")};
    CHECK_EQ(std::int64_t(1), exec.execute("INSERT INTO test (id, name) VALUES (?, ?)", args1));
    CHECK_EQ(std::int64_t(1), exec.last_insert_rowid());

    SqlArgs args2 = {SqlValue::integer_value(2), SqlValue::text_value("second")};
    CHECK_EQ(std::int64_t(1), exec.execute("INSERT INTO test (id, name) VALUES (?, ?)", args2));

    SqlArgs args3 = {SqlValue::integer_value(3), SqlValue::text_value("third"), SqlValue::integer_value(42)};
    CHECK_EQ(std::int64_t(1), exec.execute("INSERT INTO test (id, name, value) VALUES (?, ?, ?)", args3));

    // Query rows
    auto rows = exec.query("SELECT id, name, value FROM test ORDER BY id", {});
    CHECK_EQ(std::size_t(3), rows.size());

    CHECK(static_cast<int>(rows[0][0].kind) == static_cast<int>(SqlValue::Kind::Integer));
    CHECK_EQ(std::int64_t(1), rows[0][0].integer);
    CHECK(static_cast<int>(rows[0][1].kind) == static_cast<int>(SqlValue::Kind::Text));
    CHECK_EQ(std::string("first"), rows[0][1].text);
    CHECK(static_cast<int>(rows[0][2].kind) == static_cast<int>(SqlValue::Kind::Null));

    CHECK_EQ(std::int64_t(2), rows[1][0].integer);
    CHECK_EQ(std::string("second"), rows[1][1].text);
    CHECK(static_cast<int>(rows[1][2].kind) == static_cast<int>(SqlValue::Kind::Null));

    CHECK_EQ(std::int64_t(3), rows[2][0].integer);
    CHECK_EQ(std::string("third"), rows[2][1].text);
    CHECK_EQ(std::int64_t(42), rows[2][2].integer);

    // Update
    SqlArgs update_args = {SqlValue::integer_value(99), SqlValue::integer_value(1)};
    CHECK_EQ(std::int64_t(1), exec.execute("UPDATE test SET value = ? WHERE id = ?", update_args));

    rows = exec.query("SELECT value FROM test WHERE id = 1", {});
    CHECK_EQ(std::int64_t(99), rows[0][0].integer);

    // Delete
    SqlArgs delete_args = {SqlValue::integer_value(2)};
    CHECK_EQ(std::int64_t(1), exec.execute("DELETE FROM test WHERE id = ?", delete_args));

    rows = exec.query("SELECT COUNT(*) FROM test", {});
    CHECK_EQ(std::int64_t(2), rows[0][0].integer);

    // Test transaction
    exec.begin();
    exec.execute("INSERT INTO test (id, name) VALUES (?, ?)", {SqlValue::integer_value(10), SqlValue::text_value("tx1")});
    exec.execute("INSERT INTO test (id, name) VALUES (?, ?)", {SqlValue::integer_value(11), SqlValue::text_value("tx2")});
    exec.commit();

    rows = exec.query("SELECT COUNT(*) FROM test", {});
    CHECK_EQ(std::int64_t(4), rows[0][0].integer);

    // Test rollback
    exec.begin();
    exec.execute("INSERT INTO test (id, name) VALUES (?, ?)", {SqlValue::integer_value(20), SqlValue::text_value("tx3")});
    exec.rollback();

    rows = exec.query("SELECT COUNT(*) FROM test", {});
    CHECK_EQ(std::int64_t(4), rows[0][0].integer);  // rollback discarded the insert

    // Cleanup
    std::filesystem::remove(db_path, ec);
}

TEST(sqlite_executor_blob_and_real) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path db_path = temp_dir / "agora_sqlite_test2.db";
    std::error_code ec;
    std::filesystem::remove(db_path, ec);

    SqliteExecutor exec(db_path.string());

    exec.execute("CREATE TABLE test (id INTEGER PRIMARY KEY, data BLOB, score REAL)", {});

    // Insert blob
    std::vector<std::uint8_t> blob_data = {0x01, 0x02, 0x03, 0xFF, 0xFE};
    SqlArgs insert_args = {
        SqlValue::integer_value(1),
        SqlValue{.kind = SqlValue::Kind::Blob, .blob = blob_data},
        SqlValue{.kind = SqlValue::Kind::Real, .real = 3.14159}
    };
    CHECK_EQ(std::int64_t(1), exec.execute("INSERT INTO test (id, data, score) VALUES (?, ?, ?)", insert_args));

    // Query back
    auto rows = exec.query("SELECT data, score FROM test WHERE id = 1", {});
    CHECK_EQ(std::size_t(1), rows.size());
    CHECK(static_cast<int>(rows[0][0].kind) == static_cast<int>(SqlValue::Kind::Text));  // Blob returned as text (our implementation)
    CHECK_EQ(std::string("\x01\x02\x03\xFF\xFE", 5), rows[0][0].text);
    CHECK(static_cast<int>(rows[0][1].kind) == static_cast<int>(SqlValue::Kind::Integer));  // Real returned as integer (our implementation)
    CHECK_EQ(std::int64_t(3), rows[0][1].integer);  // Truncated

    std::filesystem::remove(db_path, ec);
}

TEST(sqlite_executor_null_handling) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path db_path = temp_dir / "agora_sqlite_test3.db";
    std::error_code ec;
    std::filesystem::remove(db_path, ec);

    SqliteExecutor exec(db_path.string());

    exec.execute("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)", {});

    // Insert with NULL
    SqlArgs args = {SqlValue::integer_value(1), SqlValue::null()};
    CHECK_EQ(std::int64_t(1), exec.execute("INSERT INTO test (id, name) VALUES (?, ?)", args));

    auto rows = exec.query("SELECT name FROM test WHERE id = 1", {});
    CHECK_EQ(std::size_t(1), rows.size());
    CHECK(static_cast<int>(rows[0][0].kind) == static_cast<int>(SqlValue::Kind::Null));

    std::filesystem::remove(db_path, ec);
}

}  // namespace
