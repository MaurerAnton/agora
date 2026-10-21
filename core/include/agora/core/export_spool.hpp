#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::export_spool {

/**
 * Port of data/ExportSpool.kt: the newline-delimited export spool, its layout index and the
 * streaming reader.
 *
 * Format: one record per line, `TYPE \t payload \n`, where TYPE is one of C/R/M/T/L. The writer
 * records byte slices (after flushing, exactly like Kotlin's `writer.flush()` before every
 * `counter.written` read) so one conversation or task block can be revisited without rescanning.
 *
 * Errors are the three Kotlin types with their exact messages: `check` -> IllegalStateException,
 * malformed records -> IOException, exhausted `Iterator.next()` -> NoSuchElementException.
 */
struct Slice {
    std::int64_t start_offset = 0;
    std::int64_t end_offset = 0;
};

struct ConversationEntry {
    std::string id;
    std::int64_t data_changed_at = 0;
    Slice slice;
};

struct Index {
    std::vector<ConversationEntry> conversations;
    std::vector<Slice> task_slices;
    /** ExportSpoolIndex.watermarks: first-insertion position, last value for duplicate ids. */
    std::vector<std::pair<std::string, std::int64_t>> watermarks() const;
};

enum class ExceptionKind { IllegalState, Io, NoSuchElement };

class SpoolException : public std::runtime_error {
public:
    SpoolException(ExceptionKind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {}
    ExceptionKind kind() const noexcept { return kind_; }
    /** "IllegalStateException" / "IOException" / "NoSuchElementException". */
    const char* kotlin_type() const noexcept;

private:
    ExceptionKind kind_;
};

/** data/ExportSpool.kt::ExportSpoolWriter. close() must be called (or the destructor does it). */
class SpoolWriter {
public:
    explicit SpoolWriter(const std::string& path);
    ~SpoolWriter();

    SpoolWriter(const SpoolWriter&) = delete;
    SpoolWriter& operator=(const SpoolWriter&) = delete;

    void write_conversation(const std::string& id, std::int64_t data_changed_at,
                            std::string_view json);
    void write_run(std::string_view json);
    void write_message(std::string_view json);
    void write_loop(std::string_view json);
    void write_task(std::string_view json);

    /** Flushes pending bytes, closes the open slice boundaries, returns the spool layout. */
    Index finish();
    void close();

private:
    void write_record(const std::string& type, std::string_view json);
    void flush_pending();
    void close_current_conversation();
    void close_current_task();

    struct Impl;
    Impl* impl_;
};

/** data/ExportSpool.kt::ExportSpoolReader. */
class SpoolReader {
public:
    struct Record {
        std::string type;
        std::string json;
    };

    explicit SpoolReader(std::string path);

    /** readRecords: clamps the slice to the file, splits on '\n', a trailing partial line still
     *  counts, malformed lines throw IOException mid-collection (records so far are kept). */
    void read_records(const Slice& slice, const std::function<void(const Record&)>& on_record) const;

    /** Pull-based iterator over the raw payloads of matching records across consecutive slices.
     *  Lines may span chunk and slice boundaries; only the current record is held. */
    class RecordIterator {
    public:
        RecordIterator(std::string path, std::vector<Slice> slices,
                       std::optional<std::string> type_filter);
        ~RecordIterator();

        RecordIterator(const RecordIterator&) = delete;
        RecordIterator& operator=(const RecordIterator&) = delete;

        bool has_next();
        std::string next();

    private:
        struct Impl;
        Impl* impl_;
    };

    RecordIterator record_iterator(std::vector<Slice> slices,
                                   std::optional<std::string> type_filter) const;

private:
    std::string path_;
};

}  // namespace agora::core::export_spool
