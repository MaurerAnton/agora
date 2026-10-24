#include "agora/core/native_conversation_graph_source.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include "agora/core/json.hpp"
#include "agora/core/native_backup_format.hpp"

namespace agora::core::native_graph {

using android_json::Exception;
using android_json::Reader;
using android_json::Source;
using android_json::Token;
using android_json::Writer;

namespace {

constexpr std::string_view kGraphFields[] = {"conversations", "runs", "messages", "loops",
                                             "tasks"};

/** UTF-16 unit offset for a byte position (kotlinx reports offsets on the decoded string). */
std::size_t u16_offset(std::string_view text, std::size_t byte_pos) {
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < byte_pos && i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            ++units;
            ++i;
        } else if ((byte & 0xE0) == 0xC0) {
            ++units;
            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            ++units;
            i += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            units += 2;
            i += 4;
        } else {
            ++units;
            ++i;
        }
    }
    return units;
}

/** The character at a byte position (one decoded codepoint), for "had 'X' instead". */
std::string char_at(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) return "";
    const auto byte = static_cast<unsigned char>(text[pos]);
    std::size_t length = 1;
    if ((byte & 0xE0) == 0xC0) length = 2;
    else if ((byte & 0xF0) == 0xE0) length = 3;
    else if ((byte & 0xF8) == 0xF0) length = 4;
    length = std::min(length, text.size() - pos);
    return std::string(text.substr(pos, length));
}

/** kotlinx JsonDecodingException: "Unexpected JSON token at offset N: <detail> at path: P". */
Exception json_decoding(std::string_view text, std::size_t byte_offset, std::string detail,
                        std::string path, const std::string* hint = nullptr) {
    std::string message = "Unexpected JSON token at offset " +
                          std::to_string(u16_offset(text, byte_offset)) + ": " + std::move(detail) +
                          " at path: " + std::move(path);
    if (hint != nullptr) message += "\n" + *hint;
    message += "\nJSON input: ";
    message += text;
    return Exception("JsonDecodingException", message);
}

constexpr std::string_view kUnknownKeyHint =
    "Use 'ignoreUnknownKeys = true' in 'Json {}' builder or '@JsonIgnoreUnknownKeys' "
    "annotation to ignore unknown keys.";

/** kotlinx MissingFieldException — one field vs a list, both forms pinned by the stand. */
Exception missing_field(std::string_view serial_name, const std::vector<std::string>& fields,
                        std::string path) {
    std::string message;
    if (fields.size() == 1) {
        message = "Field '" + fields[0] + "' is required for type with serial name '" +
                  std::string(serial_name) + "', but it was missing at path: " + path;
    } else {
        message = "Fields [";
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i) message += ", ";
            message += fields[i];
        }
        message += "] are required for type with serial name '" + std::string(serial_name) +
                   "', but they were missing at path: " + path;
    }
    return Exception("MissingFieldException", message);
}

/** Kotlin String.toLongOrNull(): optional sign, all decimal digits, no overflow. */
std::optional<long long> kotlin_to_long(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        i = 1;
    }
    if (i >= text.size()) return std::nullopt;
    const unsigned long long limit =
        negative ? 9223372036854775808ull : 9223372036854775807ull;
    unsigned long long accumulator = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return std::nullopt;
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (accumulator > (limit - digit) / 10) return std::nullopt;  // overflow
        accumulator = accumulator * 10 + digit;
    }
    if (negative) {
        if (accumulator == 9223372036854775808ull) return std::numeric_limits<long long>::min();
        return -static_cast<long long>(accumulator);
    }
    return static_cast<long long>(accumulator);
}

/** java.io.FileInputStream over a local file (the stand double's stream contract). */
class FileSource final : public Source {
public:
    static std::unique_ptr<FileSource> open(const std::filesystem::path& path) {
        auto stream = std::make_unique<std::ifstream>(path, std::ios::binary);
        if (!stream->is_open()) {
            throw Exception("IOException",
                            path.string() + " (No such file or directory)");
        }
        std::unique_ptr<FileSource> source(new FileSource());
        source->stream_ = std::move(stream);
        return source;
    }

    long long read(char* buffer, long long capacity) override {
        if (capacity <= 0) return 0;
        if (!stream_) return -1;
        stream_->read(buffer, capacity);
        const std::streamsize got = stream_->gcount();
        if (got > 0) return static_cast<long long>(got);
        return -1;
    }

    void close() override {
        if (stream_) stream_->close();
    }

private:
    FileSource() = default;
    std::unique_ptr<std::ifstream> stream_;
};

}  // namespace

// -----------------------------------------------------------------------------------------------
// Stream
// -----------------------------------------------------------------------------------------------

struct NativeConversationGraphSource::Stream::State {
    bool closed = false;
    bool spool = false;
    std::unique_ptr<Source> legacy;
    struct Piece {
        std::string bytes;   // non-empty only for the separator/closing pieces
        Source* src = nullptr;
        std::size_t pos = 0;
    };
    std::vector<Piece> pieces;
    std::vector<std::unique_ptr<Source>> owned;
    std::size_t piece = 0;
};

long long NativeConversationGraphSource::Stream::read(char* buffer, long long capacity) {
    if (state_ == nullptr || capacity <= 0) return state_ == nullptr ? -1 : 0;
    if (state_->closed) {
        if (state_->spool) {
            // Pinned JDK contract: SequenceInputStream.close() nulls the current element, so a
            // read after close reports EOF even when pieces were never drained.
            return -1;
        }
        // A FileInputStream-style stream throws after close — the delegate decides (pinned).
        return state_->legacy->read(buffer, capacity);
    }
    if (!state_->spool) return state_->legacy->read(buffer, capacity);

    while (true) {
        if (state_->piece >= state_->pieces.size()) return -1;
        auto& piece = state_->pieces[state_->piece];
        if (piece.src == nullptr) {
            const std::size_t remaining = piece.bytes.size() - piece.pos;
            if (remaining == 0) {
                ++state_->piece;
                continue;
            }
            const std::size_t take =
                std::min<std::size_t>(remaining, static_cast<std::size_t>(capacity));
            std::memcpy(buffer, piece.bytes.data() + piece.pos, take);
            piece.pos += take;
            return static_cast<long long>(take);
        }
        const long long got = piece.src->read(buffer, capacity);
        if (got > 0) return got;
        if (got < 0) {
            ++state_->piece;
            continue;
        }
        return 0;
    }
}

std::string NativeConversationGraphSource::Stream::read_all() {
    std::string out;
    char scratch[8192];
    long long got = 0;
    while ((got = read(scratch, sizeof(scratch))) >= 0) {
        out.append(scratch, static_cast<std::size_t>(got));
    }
    return out;
}

// -----------------------------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------------------------

NativeConversationGraphSource::~NativeConversationGraphSource() { close(); }

NativeConversationGraphSource::NativeConversationGraphSource(
    NativeConversationGraphSource&& other) noexcept
    : archive_(other.archive_),
      legacy_entry_(std::move(other.legacy_entry_)),
      spool_dir_(std::move(other.spool_dir_)),
      has_spool_(other.has_spool_),
      closed_(other.closed_),
      handed_out_(std::move(other.handed_out_)) {
    other.archive_ = nullptr;
    other.has_spool_ = false;
    other.closed_ = true;  // the moved-from object must not touch our spool
}

NativeConversationGraphSource& NativeConversationGraphSource::operator=(
    NativeConversationGraphSource&& other) noexcept {
    if (this != &other) {
        close();
        archive_ = other.archive_;
        legacy_entry_ = std::move(other.legacy_entry_);
        spool_dir_ = std::move(other.spool_dir_);
        has_spool_ = other.has_spool_;
        closed_ = other.closed_;
        handed_out_ = std::move(other.handed_out_);
        other.archive_ = nullptr;
        other.has_spool_ = false;
        other.closed_ = true;
    }
    return *this;
}

void NativeConversationGraphSource::close() {
    if (closed_) return;
    closed_ = true;
    for (const auto& state : handed_out_) {
        if (state->closed) continue;
        state->closed = true;
        if (state->spool) {
            for (auto& piece : state->owned) {
                try {
                    piece->close();
                } catch (...) {
                }
            }
        } else if (state->legacy) {
            try {
                state->legacy->close();
            } catch (...) {
            }
        }
    }
    handed_out_.clear();
    if (has_spool_) {
        std::error_code ignored;
        std::filesystem::remove_all(spool_dir_, ignored);
        // Kotlin falls back to deleteOnExit() here, which has no observable trace effect.
    }
}

NativeConversationGraphSource::Stream NativeConversationGraphSource::open_stream() {
    auto state = std::make_shared<Stream::State>();
    if (!legacy_entry_.empty()) {
        state->spool = false;
        state->legacy = archive_->stream(legacy_entry_);
        if (!state->legacy) {
            throw Exception("IllegalStateException", "Required value was null.");  // checkNotNull
        }
    } else if (!has_spool_ || closed_) {
        throw Exception("IllegalStateException", "Required value was null.");  // checkNotNull(spool)
    } else {
        state->spool = true;
        try {
            for (std::size_t index = 0; index < std::size(kGraphFields); ++index) {
                const std::string field{kGraphFields[index]};
                Stream::State::Piece separator;
                separator.bytes = std::string(index == 0 ? "{" : ",") + "\"" + field + "\":";
                state->pieces.push_back(std::move(separator));

                const auto part = spool_dir_ / (field + ".json");
                auto file = FileSource::open(part);
                Stream::State::Piece file_piece;
                file_piece.src = file.get();
                state->owned.push_back(std::move(file));
                state->pieces.push_back(std::move(file_piece));
            }
            Stream::State::Piece closing;
            closing.bytes = "}";
            state->pieces.push_back(std::move(closing));
        } catch (...) {
            for (auto& piece : state->owned) {
                try {
                    piece->close();
                } catch (...) {
                }
            }
            throw;
        }
    }
    handed_out_.push_back(state);
    return Stream(std::move(state));
}


// -----------------------------------------------------------------------------------------------
// kotlinx strict decode of conv/index.json
// -----------------------------------------------------------------------------------------------

namespace {

struct IndexEntry {
    std::string id;
    std::int64_t data_changed_at = 0;
    std::string entry;
    std::vector<std::string> media_entries;
};

struct GraphIndex {
    std::vector<IndexEntry> conversations;
};

std::size_t skip_ws(std::string_view text, std::size_t from) {
    std::size_t pos = from;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
        else break;
    }
    return pos;
}

/**
 * The byte offset kotlinx reports for an unknown key: the key NAME's first character (just
 * after its opening quote) — pinned by the stand's 05b trace (offset 23 for `"bogus"`).
 */
std::size_t key_offset(std::string_view text, std::size_t from) {
    std::size_t pos = from;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') ++pos;
        else break;
    }
    if (pos < text.size() && text[pos] == '"') return pos + 1;
    return pos;
}

const json::Value* member_of(const json::Value& object, std::string_view key) {
    for (const auto& member : object.members) {
        if (member.key == key) return &member.value;
    }
    return nullptr;
}

bool looks_like_long(const json::Value& value) {
    if (value.kind != json::Kind::Literal) return false;
    return kotlin_to_long(value.text).has_value();
}

void decode_index_item(const json::Value& item, std::string_view text, const std::string& path,
                       IndexEntry& out) {
    if (item.kind != json::Kind::Object) {
        const std::size_t pos = skip_ws(text, item.begin);
        throw json_decoding(text, pos,
                            "Expected start of the object '{', but had '" + char_at(text, pos) +
                                "' instead",
                            path);
    }
    // Unknown keys first (kotlinx reports them as they are encountered, with a cursor walk
    // over the raw text because json::Value members carry no key offset).
    std::size_t cursor = skip_ws(text, item.begin) + 1;  // just past '{'
    for (const auto& member : item.members) {
        if (member.key != "id" && member.key != "dataChangedAt" && member.key != "entry" &&
            member.key != "mediaEntries") {
            const std::string hint{kUnknownKeyHint};
            throw json_decoding(text, key_offset(text, cursor),
                                "Encountered an unknown key '" + member.key + "'", path, &hint);
        }
        cursor = member.value.end;
    }
    const json::Value* id = member_of(item, "id");
    const json::Value* changed = member_of(item, "dataChangedAt");
    const json::Value* entry = member_of(item, "entry");
    const json::Value* media = member_of(item, "mediaEntries");

    const std::string serial = "com.newoether.agora.data.NativeConversationIndexEntry";
    std::vector<std::string> missing;
    if (id == nullptr) missing.push_back("id");
    if (changed == nullptr) missing.push_back("dataChangedAt");
    if (entry == nullptr) missing.push_back("entry");
    if (!missing.empty()) throw missing_field(serial, missing, path);

    if (id->kind != json::Kind::String || entry->kind != json::Kind::String || !looks_like_long(*changed)) {
        const json::Value& bad = id->kind != json::Kind::String
                                     ? *id
                                     : (entry->kind != json::Kind::String ? *entry : *changed);
        throw json_decoding(text, bad.begin, "Unexpected value", path);
    }
    out.id = id->text;
    out.data_changed_at = *kotlin_to_long(changed->text);
    out.entry = entry->text;
    if (media != nullptr) {
        if (media->kind != json::Kind::Array) {
            throw json_decoding(text, media->begin,
                                "Expected start of the array '[', but had '" +
                                    char_at(text, skip_ws(text, media->begin)) + "' instead",
                                path);
        }
        for (const auto& element : media->elements) {
            if (element.kind != json::Kind::String) {
                throw json_decoding(text, element.begin, "Unexpected value", path);
            }
            out.media_entries.push_back(element.text);
        }
    }
}

GraphIndex decode_index_strict(const std::string& text) {
    const std::size_t first = skip_ws(text, 0);
    if (first >= text.size()) {
        throw json_decoding(text, first,
                            "Expected start of the object '{', but had 'end of input' instead",
                            "$");
    }
    if (text[first] != '{') {
        throw json_decoding(text, first,
                            "Expected start of the object '{', but had '" + char_at(text, first) +
                                "' instead",
                            "$");
    }

    const json::ParseResult parsed = json::parse(text);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        const std::size_t at = parsed.error_position;
        throw json_decoding(text, at, "Malformed JSON", "$");
    }
    const json::Value& root = *parsed.value;

    // Unknown top-level keys (pinned offset: the key's opening quote).
    std::size_t cursor = first + 1;  // just past '{'
    for (const auto& member : root.members) {
        const std::size_t key_at = key_offset(text, cursor);
        if (member.key != "conversations") {
            std::string hint{kUnknownKeyHint};
            throw json_decoding(text, key_at,
                                "Encountered an unknown key '" + member.key + "'", "$", &hint);
        }
        cursor = member.value.end;
    }

    const json::Value* conversations = member_of(root, "conversations");
    if (conversations == nullptr) {
        throw missing_field("com.newoether.agora.data.NativeConversationIndex", {"conversations"},
                            "$");
    }
    if (conversations->kind != json::Kind::Array) {
        const std::size_t pos = skip_ws(text, conversations->begin);
        throw json_decoding(text, conversations->begin,
                            "Expected start of the array '[', but had '" + char_at(text, pos) +
                                "' instead",
                            "$.conversations");
    }

    GraphIndex index;
    for (std::size_t i = 0; i < conversations->elements.size(); ++i) {
        IndexEntry item;
        decode_index_item(conversations->elements[i], text,
                          "$.conversations[" + std::to_string(i) + "]", item);
        index.conversations.push_back(std::move(item));
    }
    return index;
}

// -----------------------------------------------------------------------------------------------
// The split: one pass per conversation item, routing arrays to their field writers
// -----------------------------------------------------------------------------------------------

void copy_value(Reader& reader, Writer& writer) {
    switch (reader.peek()) {
        case Token::BeginArray:
            reader.begin_array();
            writer.begin_array();
            while (reader.has_next()) copy_value(reader, writer);
            reader.end_array();
            writer.end_array();
            break;
        case Token::BeginObject:
            reader.begin_object();
            writer.begin_object();
            while (reader.has_next()) {
                writer.name(reader.next_name());
                copy_value(reader, writer);
            }
            reader.end_object();
            writer.end_object();
            break;
        case Token::String:
            writer.value_string(reader.next_string());
            break;
        case Token::Number: {
            const std::string raw = reader.next_string();
            if (const std::optional<long long> as_long = kotlin_to_long(raw)) {
                writer.value_long(*as_long);
            } else {
                writer.value_double(android_json::java_parse_double(raw));
            }
            break;
        }
        case Token::Boolean:
            writer.value_bool(reader.next_boolean());
            break;
        case Token::Null:
            reader.next_null();
            writer.value_null();
            break;
        default:
            throw Exception("IllegalStateException",
                            "Unexpected JSON token " + token_name(reader.peek()));
    }
}

void split_item(Source& stream, std::map<std::string, Writer>& writers) {
    Reader reader(stream);
    reader.begin_object();
    while (reader.has_next()) {
        const std::string field = reader.next_name();
        const auto it = writers.find(field);
        if (it == writers.end() || field == "tasks") {
            reader.skip_value();
            continue;
        }
        reader.begin_array();
        while (reader.has_next()) copy_value(reader, it->second);
        reader.end_array();
    }
    reader.end_object();
    android_json::read_to_end(stream);
}

void copy_array_field(Source& stream, const std::string& field, Writer& writer) {
    Reader reader(stream);
    reader.begin_object();
    while (reader.has_next()) {
        if (reader.next_name() == field) {
            reader.begin_array();
            while (reader.has_next()) copy_value(reader, writer);
            reader.end_array();
        } else {
            reader.skip_value();
        }
    }
    reader.end_object();
    android_json::read_to_end(stream);
}

std::filesystem::path create_temp_directory(const std::filesystem::path& parent,
                                            std::string_view prefix) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    for (int counter = 0;; ++counter) {
        const auto candidate = parent / (std::string(prefix) + std::to_string(counter));
        if (std::filesystem::create_directory(candidate)) return candidate;
        if (counter > 1000000) {
            throw Exception("IOException", "Unable to create temporary directory");
        }
    }
}

}  // namespace

NativeConversationGraphSource NativeConversationGraphSource::open(EntrySource& archive, int version,
                                                                 const std::filesystem::path& cache_dir) {
    NativeConversationGraphSource source;
    source.archive_ = &archive;

    if (version < 5) {
        if (!archive.has(native_backup::kConversationsEntry)) {
            throw Exception("IllegalArgumentException", "Failed requirement.");  // require(...)
        }
        source.legacy_entry_ = std::string(native_backup::kConversationsEntry);
        return source;
    }

    const std::optional<std::string> raw_index =
        archive.bytes(native_backup::kConversationIndexEntry);
    if (!raw_index.has_value()) {
        throw Exception("IllegalStateException",
                        std::string(native_backup::kConversationIndexEntry) + " is missing");
    }
    const GraphIndex index = decode_index_strict(*raw_index);

    source.spool_dir_ = create_temp_directory(cache_dir, "agora-import-v5-");
    source.has_spool_ = true;

    try {
        std::set<std::string> seen;
        for (const auto& item : index.conversations) {
            if (!seen.insert(item.id).second) {
                throw Exception("IllegalArgumentException", "Duplicate conversation id in index");
            }
            const std::string expected = native_backup::conversation_entry(item.id);
            if (item.entry != expected) {
                throw Exception("IllegalArgumentException",
                                "Index entry " + item.entry + " does not match id " + item.id);
            }
        }

        std::map<std::string, Writer> writers;
        for (const auto field : kGraphFields) {
            writers[std::string(field)].begin_array();
        }

        for (const auto& item : index.conversations) {
            auto stream = archive.stream(item.entry);
            if (!stream) {
                throw Exception("IllegalStateException", item.entry + " is missing");
            }
            try {
                split_item(*stream, writers);
            } catch (...) {
                stream->close();
                throw;
            }
            stream->close();
        }

        auto tasks_stream = archive.stream(native_backup::kTasksEntry);
        if (!tasks_stream) {
            throw Exception("IllegalStateException",
                            std::string(native_backup::kTasksEntry) + " is missing");
        }
        try {
            copy_array_field(*tasks_stream, "tasks", writers.at("tasks"));
        } catch (...) {
            tasks_stream->close();
            throw;
        }
        tasks_stream->close();

        for (const auto field : kGraphFields) {
            Writer& writer = writers.at(std::string(field));
            writer.end_array();
            writer.close();
        }
        for (const auto field : kGraphFields) {
            const auto part = source.spool_dir_ / (std::string(field) + ".json");
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            const std::string& bytes = writers.at(std::string(field)).str();
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        return source;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(source.spool_dir_, ignored);
        throw;
    }
}

}  // namespace agora::core::native_graph
