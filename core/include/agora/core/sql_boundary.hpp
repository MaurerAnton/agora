#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core {

// ---- Kotlin validation failures ----------------------------------------------------------

/**
 * A Kotlin `require`/`check`/`checkNotNull` failure surfaced with the JVM exception class,
 * because the golden drivers record `{"error": class, "message": message}` for every step.
 */
class KotlinError : public std::runtime_error {
public:
    KotlinError(std::string class_name, std::string message)
        : std::runtime_error(std::move(message)), class_name_(std::move(class_name)) {}

    const std::string& class_name() const { return class_name_; }

private:
    std::string class_name_;
};

/** Kotlin require(value) / require(value) { message }. */
inline void kotlin_require(bool satisfied, std::string message = "Failed requirement.") {
    if (!satisfied) throw KotlinError("IllegalArgumentException", std::move(message));
}

/** Kotlin check(value) / checkNotNull(value) { message }. */
inline void kotlin_check(bool satisfied, std::string message = "Check failed.") {
    if (!satisfied) throw KotlinError("IllegalStateException", std::move(message));
}

// ---- SQL execution boundary --------------------------------------------------------------

/** A constraint rejection (SQLITE_CONSTRAINT): Room's @Upsert falls back to update-by-PK. */
class SqlConstraintError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Any other SQLite failure — only reachable through a bug in the ported SQL. */
class SqlError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};


/** A bound parameter or result cell with the SQLite runtime type preserved. */
struct SqlValue {
    enum class Kind {
        Null,
        Text,
        Integer,
        Real,
        Blob,
    };

    Kind kind = Kind::Null;
    std::string text;
    std::int64_t integer = 0;
    double real = 0.0;
    std::vector<std::uint8_t> blob;

    static SqlValue null() {
        return SqlValue{};
    }

    static SqlValue text_value(std::string value) {
        SqlValue out;
        out.kind = Kind::Text;
        out.text = std::move(value);
        return out;
    }

    static SqlValue integer_value(std::int64_t value) {
        SqlValue out;
        out.kind = Kind::Integer;
        out.integer = value;
        return out;
    }

    static SqlValue bool_value(bool value) { return integer_value(value ? 1 : 0); }

    bool is_null() const { return kind == Kind::Null; }

    const std::string& as_text() const {
        if (kind != Kind::Text) throw SqlError("expected a TEXT value");
        return text;
    }

    std::int64_t as_integer() const {
        if (kind != Kind::Integer && kind != Kind::Real) throw SqlError("expected an INTEGER value");
        return integer;
    }

    std::vector<std::uint8_t> as_blob() const {
        if (kind != Kind::Blob) throw SqlError("expected a BLOB value");
        return blob;
    }

    std::optional<std::string> text_or_null() const {
        if (kind == Kind::Null) return std::nullopt;
        return as_text();
    }

    std::optional<std::int64_t> integer_or_null() const {
        if (kind == Kind::Null) return std::nullopt;
        return as_integer();
    }
};

using SqlArgs = std::vector<SqlValue>;
using SqlRow = std::vector<SqlValue>;

/**
 * The SQLite execution boundary. Room is replaced by this surface the same way the clone stand
 * replaced the file system: the port owns the SQL text (transcribed from the @Query annotations)
 * and every conditional sequence, and the caller supplies the database.
 */
class SqlExecutor {
public:
    virtual ~SqlExecutor() = default;

    /** Runs a statement without result rows; returns the number of changed rows. */
    virtual std::int64_t execute(const std::string& sql, const SqlArgs& args) = 0;

    /** Runs a query; rows carry their cells in column order. */
    virtual std::vector<SqlRow> query(const std::string& sql, const SqlArgs& args) = 0;

    /** sqlite3_last_insert_rowid() for this connection. */
    virtual std::int64_t last_insert_rowid() = 0;

    /** SQLite transaction control; begin/commit/rollback arrive in matched pairs. */
    virtual void begin() = 0;
    virtual void commit() = 0;
    virtual void rollback() = 0;
};

/**
 * Room's transaction nesting: every level has to be marked successful, one unmarked level rolls
 * the whole stack back (RoomDatabase.beginTransaction/setTransactionSuccessful/endTransaction).
 */
class TransactionStack {
public:
    explicit TransactionStack(SqlExecutor& sql) : sql_(sql) {}

    void begin() {
        if (levels_.empty()) {
            aborted_ = false;
            sql_.begin();
        }
        levels_.push_back(false);
    }

    void mark_successful() {
        kotlin_check(!levels_.empty(), "RoomDatabase: no transaction in progress");
        levels_.back() = true;
    }

    void end() {
        kotlin_check(!levels_.empty(), "RoomDatabase: no transaction in progress");
        const bool successful = levels_.back();
        levels_.pop_back();
        if (!successful) aborted_ = true;
        if (levels_.empty()) {
            if (aborted_) {
                sql_.rollback();
            } else {
                sql_.commit();
            }
            aborted_ = false;
            levels_.clear();
        }
    }

private:
    SqlExecutor& sql_;
    std::vector<bool> levels_;
    bool aborted_ = false;
};

}  // namespace agora::core
