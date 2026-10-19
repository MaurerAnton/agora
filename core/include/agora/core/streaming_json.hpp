#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::streaming_json {

// Port of ui/chat/message/StreamingJsonDocument.kt and
// ui/chat/message/StreamingToolArgumentHints.kt. Both sources are pure Kotlin over String; the
// port keeps every index, length and slice in UTF-16 code units, exactly like the JVM, because
// StreamingJsonDocument.errorOffset is a UI cursor into the UTF-16 argument string.
//
// The parser accepts only JSON grammar and never invents missing syntax: truncation is
// INCOMPLETE, a closed document is COMPLETE, an impossible prefix is INVALID. Open leaves keep
// their partial content so the UI can render while a tool call is still streaming.

enum class Status {
    Incomplete,
    Complete,
    Invalid,
};

enum class ScalarKind {
    String,
    Number,
    Boolean,
    Null,
};

/** Mirrors the ToolKind enum extracted from ui/chat/message/ToolPresentation.kt. */
enum class ToolKind {
    MemoryList,
    MemoryRead,
    MemoryCreate,
    MemoryEdit,
    MemoryDelete,
    MemoryUpdateActive,
    SkillList,
    SkillRead,
    SkillCreate,
    SkillEdit,
    SkillDelete,
    WebSearch,
    WebFetch,
    ConversationSearch,
    ConversationList,
    ConversationRead,
    ShellList,
    ShellExecute,
    ShellJobList,
    ShellJobGet,
    ShellJobWait,
    ShellJobStop,
    FileRead,
    FileWrite,
    FileEdit,
    FileGlob,
    FileGrep,
    ImageView,
    ImageGenerate,
    TaskCreate,
    TaskList,
    TaskDelete,
    LoopStart,
    LoopStop,
    AskUser,
    Mcp,
    Unknown,
};

/** Parses a ToolKind by its Kotlin enum name (e.g. "MEMORY_READ"); nullopt for unknown names. */
std::optional<ToolKind> tool_kind_from_name(std::string_view name);

struct Entry;

/** One JSON node. Object entries keep source order; open leaves keep partial content. */
struct Node {
    enum class Kind {
        Object,
        Array,
        Scalar,
    };

    Kind kind = Kind::Scalar;
    bool complete = false;

    // Scalar
    std::u16string content;
    ScalarKind scalar_kind = ScalarKind::String;

    // Object
    std::vector<Entry> entries;

    // Array
    std::vector<Node> values;
};

/** One object member; `value` is null exactly when Kotlin has a null node (open key/colon). */
struct Entry {
    std::u16string key;
    bool key_complete = false;
    std::optional<Node> value;
};

struct Document {
    std::optional<Node> root;
    Status status = Status::Incomplete;
    std::optional<int> error_offset;
    std::vector<Node> roots;
};

/**
 * Extracted pieces of an unfinished tool-call argument document (see the Kotlin data class of
 * the same shape). Strings are UTF-8 at this boundary.
 */
struct Hints {
    std::optional<std::string> subject;
    std::optional<std::string> server;
    std::optional<int> count;
    std::optional<std::string> operation;
    std::optional<std::string> destination;
};

/** StreamingJsonParser.parse: prefix-aware parse of a UTF-8 argument buffer. */
Document parse(std::string_view source_utf8);

/**
 * normalizeToolSummarySubject: collapse ASCII regex `\s+` runs to one space, Kotlin-trim, drop
 * empty results, then truncate to max_characters UTF-16 units with an ellipsis.
 *
 * The Kotlin `require(maxCharacters > 0)` precondition is the caller's contract; every app call
 * site passes MAX_TOOL_SUMMARY_SUBJECT_CHARS (120) or a positive width.
 */
std::optional<std::string> normalize_subject(const std::optional<std::string>& value,
                                             int max_characters = 120);

/**
 * StreamingToolArgumentHintResolver.resolve: parse at most 4096 UTF-16 units of the arguments and
 * lift the identifying fields for the compact tool card.
 */
Hints resolve(ToolKind kind, const std::optional<std::string>& raw_arguments_utf8);

/** Kotlin data-class toString() of StreamingJsonDocument, byte for byte (UTF-8). */
std::string describe(const Document& document);

/** Kotlin data-class toString() of StreamingToolArgumentHints. */
std::string describe(const Hints& hints);

}  // namespace agora::core::streaming_json
