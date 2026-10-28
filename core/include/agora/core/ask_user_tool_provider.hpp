#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/shell_tool_provider.hpp"

// tool/AskUserToolProvider.kt: the ask_user tool — a decision handed back to the user instead
// of guessed. Blocking calls suspend until the bar answers (the port's Host seam blocks the
// calling thread); non-blocking calls return a queued envelope and the answer later travels as
// a user message, so they require a conversation.

namespace agora::core::askuser {

/** AskUserController.Answer. */
struct Answer {
    std::vector<std::string> choices;
    std::optional<std::string> text;
    bool answered = false;

    /** AskUserController.Answer.Unanswered. */
    static Answer unanswered() { return Answer{{}, std::nullopt, false}; }
};

/**
 * The AskUserController seam (viewmodel/AskUserController.kt). Kotlin's Request object is
 * reduced to its id: that is the only field the provider reads (awaitAnswer/abandon).
 * The Kotlin awaitAnswer suspends; here await_answer blocks the caller (the core execute()
 * is synchronous) — production bridges it to the coroutine waiter, tests script it.
 */
class Host {
public:
    virtual ~Host() = default;

    /** AskUserController.newSetId — one id shared by a multi-question card. */
    virtual std::int64_t new_set_id() = 0;

    /** AskUserController.open; returns the request id the bar now carries. */
    virtual std::int64_t open(const std::optional<std::string>& conversation_id,
                              const std::string& question,
                              const std::vector<std::string>& options,
                              bool allow_multiple, bool blocking,
                              const std::optional<std::int64_t>& set_id) = 0;

    /** AskUserController.awaitAnswer (blocking here). */
    virtual Answer await_answer(std::int64_t request_id) = 0;

    /** AskUserController.abandon. */
    virtual void abandon(std::int64_t request_id) = 0;
};

class AskUserToolProvider final : public shell_provider::ToolProvider {
public:
    /** Enough for a set of related decisions, few enough that the bar stays answerable. */
    static constexpr std::int32_t kMaxQuestions = 5;

    explicit AskUserToolProvider(Host& host);

    std::vector<openai::ToolDefinition> definitions(
        const shell_provider::GenerationContext& ctx) const override;

    /** Kotlin execute never reads the name (the pipeline gates it with handles()). */
    std::string execute(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx) override;

    bool handles(const std::string& name) const override;

private:
    /** One question as the model asked it, before it reaches the bar. */
    struct PendingQuestion {
        std::string question;
        std::vector<std::string> options;
        bool allow_multiple = false;
    };

    /** ParsedQuestions: Valid carries the questions; Invalid the failure envelope fields. */
    struct ParsedQuestions {
        bool valid = false;
        std::string code;
        std::string detail;
        std::vector<PendingQuestion> questions;
    };

    /** parseQuestions: single vs set form, refused together, bounds and per-entry checks. */
    static ParsedQuestions parse_questions(const json::Value& args);

    /** `(element as? JsonPrimitive)?.content?.takeIf { it.isNotBlank() }`. */
    static std::optional<std::string> string_or_null(const json::Value* element);

    /** `(source["options"] as? JsonArray)?.mapNotNull{…content}?.filter{…}?.distinct()`. */
    static std::vector<std::string> options_of(const json::Value& source);

    /** `source["allow_multiple"]?.jsonPrimitive?.booleanOrNull ?: false` (throws when not primitive). */
    static bool allow_multiple_of(const json::Value& source);

    static std::string failure(const std::string& code, const std::string& detail);
    static std::string queued_result(std::size_t question_count);
    static std::string single_result(const Answer& answer);
    static std::string group_result(const std::vector<PendingQuestion>& questions,
                                    const std::vector<Answer>& answers);

    Host& host_;
};

}  // namespace agora::core::askuser