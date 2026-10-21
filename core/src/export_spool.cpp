// Port of data/ExportSpool.kt.
//
// Byte-for-byte spool writing (with Kotlin's flush points driving the offset counter), the
// clamping/error semantics of readRecords and the chunk/slice-crossing state machine of
// SpoolRecordIterator. Every non-local exit is one of the three Kotlin exception types with its
// pinned message; the module never re-frames records (the newline is the frame).
#include "agora/core/export_spool.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::export_spool {

namespace {

constexpr std::int64_t kReadChunkBytes = 64 * 1024;
constexpr char kTab = '\t';
constexpr char kNewline = '\n';

std::int64_t file_size(std::ifstream& file) {
    file.seekg(0, std::ios::end);
    const std::int64_t size = static_cast<std::int64_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    return size;
}

}  // namespace

const char* SpoolException::kotlin_type() const noexcept {
    switch (kind_) {
        case ExceptionKind::IllegalState:
            return "IllegalStateException";
        case ExceptionKind::Io:
            return "IOException";
        case ExceptionKind::NoSuchElement:
            return "NoSuchElementException";
    }
    return "IllegalStateException";
}

std::vector<std::pair<std::string, std::int64_t>> Index::watermarks() const {
    std::vector<std::pair<std::string, std::int64_t>> out;
    for (const ConversationEntry& conversation : conversations) {
        bool replaced = false;
        for (auto& entry : out) {
            if (entry.first == conversation.id) {
                entry.second = conversation.data_changed_at;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.emplace_back(conversation.id, conversation.data_changed_at);
    }
    return out;
}

// ----------------------------------------------------------------------------- writer

struct SpoolWriter::Impl {
    std::ofstream file;
    std::string pending;       // the BufferedWriter layer above the counter
    std::int64_t written = 0;  // CountingOutputStream.written (flushed bytes only)
    std::optional<std::string> current_id;
    std::int64_t current_data_changed_at = 0;
    std::int64_t current_start_offset = 0;
    bool task_open = false;
    std::int64_t task_start_offset = 0;
    std::vector<ConversationEntry> conversations;
    std::vector<Slice> task_ranges;
    bool closed = false;

    explicit Impl(const std::string& path) : file(path, std::ios::binary | std::ios::trunc) {
        if (!file.is_open()) {
            throw SpoolException(ExceptionKind::Io, "cannot open spool file for writing");
        }
    }
};

SpoolWriter::SpoolWriter(const std::string& path) : impl_(new Impl(path)) {}

SpoolWriter::~SpoolWriter() {
    try {
        close();
    } catch (...) {
    }
    delete impl_;
}

void SpoolWriter::flush_pending() {
    if (impl_->pending.empty()) return;
    impl_->file.write(impl_->pending.data(),
                      static_cast<std::streamsize>(impl_->pending.size()));
    impl_->written += static_cast<std::int64_t>(impl_->pending.size());
    impl_->pending.clear();
}

void SpoolWriter::write_record(const std::string& type, std::string_view json) {
    impl_->pending += type;
    impl_->pending.push_back(kTab);
    impl_->pending.append(json);
    impl_->pending.push_back(kNewline);
}

void SpoolWriter::close_current_conversation() {
    if (!impl_->current_id.has_value()) return;
    flush_pending();
    impl_->conversations.push_back(ConversationEntry{
        *impl_->current_id,
        impl_->current_data_changed_at,
        Slice{impl_->current_start_offset, impl_->written},
    });
    impl_->current_id.reset();
}

void SpoolWriter::close_current_task() {
    if (!impl_->task_open) return;
    flush_pending();
    impl_->task_ranges.push_back(Slice{impl_->task_start_offset, impl_->written});
    impl_->task_open = false;
}

void SpoolWriter::write_conversation(const std::string& id, std::int64_t data_changed_at,
                                     std::string_view json) {
    close_current_conversation();
    close_current_task();
    impl_->current_id = id;
    impl_->current_data_changed_at = data_changed_at;
    impl_->current_start_offset = impl_->written;
    write_record("C", json);
}

void SpoolWriter::write_run(std::string_view json) {
    if (!impl_->current_id.has_value()) {
        throw SpoolException(ExceptionKind::IllegalState,
                             "Spool record R emitted outside a conversation");
    }
    write_record("R", json);
}

void SpoolWriter::write_message(std::string_view json) {
    if (!impl_->current_id.has_value()) {
        throw SpoolException(ExceptionKind::IllegalState,
                             "Spool record M emitted outside a conversation");
    }
    write_record("M", json);
}

void SpoolWriter::write_loop(std::string_view json) {
    if (!impl_->current_id.has_value()) {
        throw SpoolException(ExceptionKind::IllegalState,
                             "Spool record L emitted outside a conversation");
    }
    write_record("L", json);
}

void SpoolWriter::write_task(std::string_view json) {
    close_current_conversation();
    if (!impl_->task_open) {
        impl_->task_open = true;
        impl_->task_start_offset = impl_->written;
    }
    write_record("T", json);
}

Index SpoolWriter::finish() {
    close_current_conversation();
    close_current_task();
    Index index;
    index.conversations = impl_->conversations;
    index.task_slices = impl_->task_ranges;
    return index;
}

void SpoolWriter::close() {
    if (impl_->closed) return;
    flush_pending();
    impl_->file.close();
    impl_->closed = true;
}

// ------------------------------------------------------------------------------ reader

SpoolReader::SpoolReader(std::string path) : path_(std::move(path)) {}

void SpoolReader::read_records(const Slice& slice,
                               const std::function<void(const Record&)>& on_record) const {
    if (slice.end_offset <= slice.start_offset) return;
    std::ifstream file(path_, std::ios::binary);
    const std::int64_t size = file_size(file);
    const std::int64_t end = std::min(slice.end_offset, size);
    if (end <= slice.start_offset) return;
    file.seekg(slice.start_offset, std::ios::beg);
    std::int64_t remaining = end - slice.start_offset;

    std::string line;
    std::vector<char> chunk(static_cast<std::size_t>(
        std::min<std::int64_t>(remaining, kReadChunkBytes)));
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::int64_t>(remaining, kReadChunkBytes));
        file.read(chunk.data(), static_cast<std::streamsize>(want));
        const std::streamsize count = file.gcount();
        if (count <= 0) break;
        remaining -= count;
        std::size_t segment_start = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
            if (chunk[i] == kNewline) {
                line.append(chunk.data() + segment_start, i - segment_start);
                const std::size_t tab = line.find(kTab);
                if (tab == std::string::npos || tab == 0) {
                    throw SpoolException(ExceptionKind::Io, "Invalid export spool record");
                }
                on_record(Record{line.substr(0, tab), line.substr(tab + 1)});
                line.clear();
                segment_start = i + 1;
            }
        }
        if (segment_start < static_cast<std::size_t>(count)) {
            line.append(chunk.data() + segment_start, static_cast<std::size_t>(count) - segment_start);
        }
    }
    if (!line.empty()) {
        const std::size_t tab = line.find(kTab);
        if (tab == std::string::npos || tab == 0) {
            throw SpoolException(ExceptionKind::Io, "Invalid export spool record");
        }
        on_record(Record{line.substr(0, tab), line.substr(tab + 1)});
    }
}

struct SpoolReader::RecordIterator::Impl {
    std::string path;
    std::vector<Slice> slices;
    std::optional<std::string> type_filter;

    std::size_t slice_index = 0;
    std::ifstream input;
    bool input_open = false;
    std::int64_t remaining = 0;
    std::string chunk;
    std::size_t chunk_pos = 0;
    std::string carry;
    std::optional<std::string> pending;
    bool exhausted = false;

    Impl(std::string p, std::vector<Slice> s, std::optional<std::string> filter)
        : path(std::move(p)), slices(std::move(s)), type_filter(std::move(filter)) {}

    void close_source() {
        if (input_open) input.close();
        input_open = false;
        remaining = 0;
    }

    bool open_next_slice() {
        while (slice_index < slices.size()) {
            const Slice slice = slices[slice_index];
            ++slice_index;
            if (slice.end_offset <= slice.start_offset) continue;
            std::ifstream file(path, std::ios::binary);
            const std::int64_t size = file_size(file);
            const std::int64_t end = std::min(slice.end_offset, size);
            if (end <= slice.start_offset) continue;
            file.seekg(slice.start_offset, std::ios::beg);
            input = std::move(file);
            input_open = true;
            remaining = end - slice.start_offset;
            return true;
        }
        return false;
    }

    bool refill() {
        while (true) {
            if (input_open) {
                if (remaining <= 0) {
                    close_source();
                    continue;
                }
                const std::size_t want = static_cast<std::size_t>(
                    std::min<std::int64_t>(remaining, kReadChunkBytes));
                std::string buffer(want, '\0');
                input.read(buffer.data(), static_cast<std::streamsize>(want));
                const std::streamsize count = input.gcount();
                if (count <= 0) {
                    close_source();
                    continue;
                }
                remaining -= count;
                buffer.resize(static_cast<std::size_t>(count));
                chunk = std::move(buffer);
                chunk_pos = 0;
                return true;
            }
            if (!open_next_slice()) return false;
        }
    }

    std::optional<std::string> next_line() {
        while (true) {
            if (chunk_pos >= chunk.size() && !refill()) {
                if (carry.empty()) return std::nullopt;
                std::string unterminated;
                unterminated.swap(carry);
                return unterminated;
            }
            const char* start = chunk.data() + chunk_pos;
            const std::size_t span = chunk.size() - chunk_pos;
            const void* found = std::memchr(start, kNewline, span);
            if (found != nullptr) {
                const auto index = static_cast<const char*>(found) - chunk.data();
                carry.append(start, static_cast<std::size_t>(index) - chunk_pos);
                chunk_pos = static_cast<std::size_t>(index) + 1;
                std::string complete;
                complete.swap(carry);
                return complete;
            }
            carry.append(start, span);
            chunk_pos = chunk.size();
        }
    }

    void advance() {
        while (true) {
            const std::optional<std::string> line = next_line();
            if (!line.has_value()) {
                exhausted = true;
                close_source();
                return;
            }
            const std::size_t tab = line->find(kTab);
            if (tab == std::string::npos || tab == 0) {
                exhausted = true;
                close_source();
                throw SpoolException(ExceptionKind::Io, "Invalid export spool record");
            }
            const std::string type = line->substr(0, tab);
            if (!type_filter.has_value() || type == *type_filter) {
                pending = line->substr(tab + 1);
                return;
            }
        }
    }
};

SpoolReader::RecordIterator::RecordIterator(std::string path, std::vector<Slice> slices,
                                            std::optional<std::string> type_filter)
    : impl_(new Impl(std::move(path), std::move(slices), std::move(type_filter))) {}

SpoolReader::RecordIterator::~RecordIterator() { delete impl_; }

bool SpoolReader::RecordIterator::has_next() {
    if (!impl_->pending.has_value() && !impl_->exhausted) impl_->advance();
    return impl_->pending.has_value();
}

std::string SpoolReader::RecordIterator::next() {
    if (!has_next()) {
        throw SpoolException(ExceptionKind::NoSuchElement, "Export spool iterator exhausted");
    }
    std::string value;
    value.swap(*impl_->pending);
    impl_->pending.reset();
    return value;
}

SpoolReader::RecordIterator SpoolReader::record_iterator(
    std::vector<Slice> slices, std::optional<std::string> type_filter) const {
    return RecordIterator(path_, std::move(slices), std::move(type_filter));
}

}  // namespace agora::core::export_spool
