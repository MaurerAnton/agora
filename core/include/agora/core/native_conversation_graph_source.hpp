#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/android_json.hpp"

namespace agora::core::native_graph {

/**
 * Port of data/NativeConversationGraphSource.kt — the v5 graph spool (index-driven split of
 * conv/items/<hex-id>.json into one JSON array file per graph field) and the v<5 legacy pass-through.
 *
 * The platform surfaces, pinned by the `native-conversation-graph-source` golden stand:
 *  - android.util.JsonReader/JsonWriter: agora::core::android_json (vendored AOSP sources).
 *  - NativeGraphEntrySource below (has/bytes/stream) — the archive boundary; the golden double
 *    is an in-memory/file-backed entry source whose streams raise IOException at a verified
 *    end (the drain fault) and java's "Stream Closed" after close.
 *  - The JDK stream contracts the traces pin: a legacy (FileInputStream-style) stream throws
 *    IOException "Stream Closed" when read after close — even fully drained — while the spool's
 *    SequenceInputStream reports EOF after close regardless of how much was read.
 *  - The index decode runs through kotlinx' strict Json (the pinned stand jar): the port
 *    reproduces MissingFieldException and JsonDecodingException class+message for the frozen
 *    shapes (missing fields with declaration-order lists, unknown keys with the
 *    ignoreUnknownKeys hint, structural mismatches with their byte offsets). Malformed index
 *    inputs outside the frozen set raise the same classes with a best-effort message.
 *  - java.io.InputStream.readToEnd() (extracted from NativeBackupArchive.kt by the stand) drains
 *    after every parse, so a checked archive failure surfaces *after* the value was read.
 */
using Exception = android_json::Exception;

/**
 * The NativeGraphEntrySource interface (data/NativeConversationGraphSource.kt): a stream is
 * java.io.InputStream — read() returns -1 at the end and may throw; close() releases it.
 */
class EntrySource {
public:
    virtual ~EntrySource() = default;
    virtual bool has(std::string_view name) = 0;
    virtual std::optional<std::string> bytes(std::string_view name) = 0;
    /** null when the entry has no openable stream. */
    virtual std::unique_ptr<android_json::Source> stream(std::string_view name) = 0;
};

/**
 * NativeConversationGraphSource: construct with open(); the graph is read through open_stream()
 * (one java.io.InputStream over the legacy entry, or the chained {"field": <part>} spool), and
 * close() closes every handed-out stream and deletes the spool — exactly like the Kotlin class,
 * the destructor closes too (RAII), which is observationally identical because close() is
 * idempotent and the Kotlin callers always close in a finally/use block.
 */
class NativeConversationGraphSource {
public:
    static NativeConversationGraphSource open(EntrySource& archive, int version,
                                              const std::filesystem::path& cache_dir);

    /** The handed-out stream: shares state with the source so close() reaches it. */
    class Stream {
    public:
        Stream() = default;

        /** java InputStream.read: >0 bytes read, -1 at the end; may throw. */
        long long read(char* buffer, long long capacity);

        /** The driver's readBytes(): drains to the end and returns the bytes. */
        std::string read_all();

    private:
        friend class NativeConversationGraphSource;
        struct State;
        explicit Stream(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    Stream open_stream();

    /** Kotlin close(): closes all handed-out streams, then deletes the spool directory. */
    void close();

    NativeConversationGraphSource() = default;
    ~NativeConversationGraphSource();
    NativeConversationGraphSource(NativeConversationGraphSource&&) noexcept;
    NativeConversationGraphSource& operator=(NativeConversationGraphSource&&) noexcept;
    NativeConversationGraphSource(const NativeConversationGraphSource&) = delete;
    NativeConversationGraphSource& operator=(const NativeConversationGraphSource&) = delete;

private:
    EntrySource* archive_ = nullptr;
    std::string legacy_entry_;
    std::filesystem::path spool_dir_;
    bool has_spool_ = false;
    bool closed_ = false;
    std::vector<std::shared_ptr<Stream::State>> handed_out_;
};

}  // namespace agora::core::native_graph
