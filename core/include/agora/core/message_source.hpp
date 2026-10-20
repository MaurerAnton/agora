#pragma once

#include <optional>
#include <string>
#include <vector>

namespace agora::core::message_source {

// Port of model/MessageSource.kt: the structured record for messages the app sends on the user's
// behalf (task prompts, loop prompts, ask_user answers). Typed messages, blocking answers and
// Compact summaries carry no source.
//
// The wire format is kotlinx.serialization JSON with encodeDefaults=false and
// ignoreUnknownKeys=true, exactly as the app configures it; the canonical encoder is what backup
// import writes back into the database.

enum class Kind {
    Task,
    Loop,
    AskUser,
};

/** Mirrors the Participant enum extracted verbatim from model/ChatMessage.kt. */
enum class Participant {
    User,
    Model,
    Error,
};

struct AskUserItem {
    std::string question;
    std::optional<std::string> answer;  // nullopt: the user left it unanswered
};

struct Source {
    Kind kind = Kind::Task;
    std::vector<AskUserItem> ask_user;
};

/** Stored text for a skipped question; the UI shows its own localized label instead. */
inline constexpr char kNoAnswer[] = "(No answer)";

/**
 * MessageSource.decode: stored JSON -> source. Blank, unreadable or future-shaped data degrades
 * to nullopt rather than failing, and the constructor precondition (only ASK_USER may carry
 * items) is enforced, so a task row with items is unreadable.
 */
std::optional<Source> decode(const std::optional<std::string>& raw);

/** MessageSource.encode: canonical stored JSON, or nullopt for a null source. */
std::optional<std::string> encode(const std::optional<Source>& source);

/**
 * MessageSource.sanitizeImported: untrusted backup data. Only a USER row can be automatic input;
 * anything unreadable imports as a plain message (nullopt). The kept value is re-encoded so the
 * stored JSON is canonical.
 */
std::optional<std::string> sanitize_imported(const std::optional<std::string>& raw,
                                             Participant participant);

/** MessageSource.forAutomationRequestKind: "task"/"loop" -> source, everything else null. */
std::optional<Source> for_automation_request_kind(const std::optional<std::string>& request_kind);

/**
 * MessageSource.askUser(items).askUserReadableText(): one question per line followed by its
 * answer, groups separated by a blank line. The Kotlin factory requires a non-empty list; every
 * call site builds it from at least one answered question.
 */
std::string ask_user_readable_text(const std::vector<AskUserItem>& items);

/** Kotlin data-class toString() of a nullable MessageSource. */
std::string describe(const std::optional<Source>& source);

}  // namespace agora::core::message_source
