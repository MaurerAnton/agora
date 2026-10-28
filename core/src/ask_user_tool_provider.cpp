#include "agora/core/ask_user_tool_provider.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace agora::core::askuser {

namespace {

namespace json = agora::core::json;

constexpr const char* kToolName = "ask_user";

// ---- JSON builders (kotlinx-compatible rendering through json::to_kotlin_string) -------------

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_literal(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

json::Value json_array(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

std::string render(const json::Value& value) { return json::to_kotlin_string(value); }

/** Kotlin Map lookup; kotlinx decodes duplicate keys last-wins. */
const json::Value* map_get(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

/** `JsonPrimitive.content` — JsonNull IS a JsonPrimitive in kotlinx (content "null"). */
std::optional<std::string> primitive_content(const json::Value& value) {
    if (value.kind == json::Kind::String || value.kind == json::Kind::Literal) return value.text;
    return std::nullopt;
}

/** kotlinx internal StringOpsKt.toBooleanStrictOrNull: exact "true"/"false", else null. */
std::optional<bool> to_boolean_strict_or_null(const std::string& content) {
    if (content == "true") return true;
    if (content == "false") return false;
    return std::nullopt;
}

}  // namespace

// ---- Private helpers (Kotlin private funs) -------------------------------------------------

std::optional<std::string> AskUserToolProvider::string_or_null(const json::Value* element) {
    if (element == nullptr) return std::nullopt;
    const std::optional<std::string> content = primitive_content(*element);
    if (!content.has_value()) return std::nullopt;  // `as? JsonPrimitive` fails on object/array
    if (json::is_blank(*content)) return std::nullopt;
    return content;
}

std::vector<std::string> AskUserToolProvider::options_of(const json::Value& source) {
    const json::Value* options = map_get(source, "options");
    if (options == nullptr || options->kind != json::Kind::Array) return {};
    std::vector<std::string> out;
    for (const json::Value& element : options->elements) {
        const std::optional<std::string> content = primitive_content(element);
        if (!content.has_value()) continue;  // mapNotNull drops non-primitives
        if (json::is_blank(*content)) continue;  // filter { it.isNotBlank() }
        if (std::find(out.begin(), out.end(), *content) == out.end()) out.push_back(*content);
    }
    return out;
}

bool AskUserToolProvider::allow_multiple_of(const json::Value& source) {
    const json::Value* element = map_get(source, "allow_multiple");
    if (element == nullptr) return false;
    const std::optional<std::string> content = primitive_content(*element);
    if (!content.has_value()) {
        // `?.jsonPrimitive` throws on JsonObject/JsonArray.
        throw std::runtime_error("Element is not a JsonPrimitive");
    }
    return to_boolean_strict_or_null(*content).value_or(false);
}

std::string AskUserToolProvider::failure(const std::string& code, const std::string& detail) {
    return render(json_object({{"error", json_string(code)}, {"detail", json_string(detail)}}));
}

std::string AskUserToolProvider::queued_result(std::size_t question_count) {
    return render(json_object(
        {{"delivery", json_string("queued")},
         {"questions", json_literal(std::to_string(question_count))},
         {"detail",
          json_string("The question is on screen. Keep working; the user's answer will arrive "
                      "as a user message. Do not guess it in the meantime.")}}));
}

std::string AskUserToolProvider::single_result(const Answer& answer) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("answered", json_literal(answer.answered ? "true" : "false"));
    if (answer.answered) {
        std::vector<json::Value> choices;
        choices.reserve(answer.choices.size());
        for (const std::string& choice : answer.choices) choices.push_back(json_string(choice));
        members.emplace_back("choices", json_array(std::move(choices)));
        if (answer.text.has_value()) members.emplace_back("text", json_string(*answer.text));
    } else {
        members.emplace_back(
            "detail",
            json_string("The user did not answer. Do not invent an answer; ask again or "
                        "continue without it."));
    }
    return render(json_object(std::move(members)));
}

std::string AskUserToolProvider::group_result(const std::vector<PendingQuestion>& questions,
                                              const std::vector<Answer>& answers) {
    std::vector<json::Value> entries;
    entries.reserve(questions.size());
    for (std::size_t index = 0; index < questions.size(); ++index) {
        // putAnswer's fields follow "question"; a missing answer yields a question-only object.
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("question", json_string(questions[index].question));
        if (index < answers.size()) {
            const Answer& answer = answers[index];
            members.emplace_back("answered", json_literal(answer.answered ? "true" : "false"));
            if (answer.answered) {
                std::vector<json::Value> choices;
                choices.reserve(answer.choices.size());
                for (const std::string& choice : answer.choices) {
                    choices.push_back(json_string(choice));
                }
                members.emplace_back("choices", json_array(std::move(choices)));
                if (answer.text.has_value()) members.emplace_back("text", json_string(*answer.text));
            } else {
                members.emplace_back(
                    "detail",
                    json_string("The user did not answer. Do not invent an answer; ask again or "
                                "continue without it."));
            }
        }
        entries.push_back(json_object(std::move(members)));
    }
    return render(json_object({{"answers", json_array(std::move(entries))}}));
}

// ---- parseQuestions -------------------------------------------------------------------------------

AskUserToolProvider::ParsedQuestions AskUserToolProvider::parse_questions(
    const json::Value& args) {
    const std::optional<std::string> single = string_or_null(map_get(args, "question"));

    // `(args["questions"] as? JsonArray).orEmpty()` — a non-array is treated as absent.
    std::vector<const json::Value*> set;
    if (const json::Value* questions = map_get(args, "questions");
        questions != nullptr && questions->kind == json::Kind::Array) {
        for (const json::Value& element : questions->elements) set.push_back(&element);
    }

    if (single.has_value() && !set.empty()) {
        return ParsedQuestions{false, "ambiguous_questions",
                               "Use question for one question or questions for a set, not both.",
                               {}};
    }
    if (single.has_value()) {
        PendingQuestion question{*single, options_of(args), allow_multiple_of(args)};
        ParsedQuestions parsed;
        parsed.valid = true;
        parsed.questions.push_back(std::move(question));
        return parsed;
    }
    if (set.empty()) {
        return ParsedQuestions{false, "no_question", "A question is required.", {}};
    }
    if (set.size() > static_cast<std::size_t>(kMaxQuestions)) {
        return ParsedQuestions{
            false, "too_many_questions",
            "Ask at most " + std::to_string(kMaxQuestions) + " questions in one call.", {}};
    }

    ParsedQuestions parsed;
    parsed.valid = true;
    parsed.questions.reserve(set.size());
    for (const json::Value* element : set) {
        if (element->kind != json::Kind::Object) {
            return ParsedQuestions{
                false, "bad_question",
                "Each entry of questions must be an object with a question field.", {}};
        }
        const std::optional<std::string> text = string_or_null(map_get(*element, "question"));
        if (!text.has_value()) {
            return ParsedQuestions{
                false, "no_question", "Each entry of questions needs a question field.", {}};
        }
        parsed.questions.push_back(
            PendingQuestion{*text, options_of(*element), allow_multiple_of(*element)});
    }
    return parsed;
}

// ---- Provider -------------------------------------------------------------------------------------

AskUserToolProvider::AskUserToolProvider(Host& host) : host_(host) {}

bool AskUserToolProvider::handles(const std::string& name) const { return name == kToolName; }

std::vector<openai::ToolDefinition> AskUserToolProvider::definitions(
    const shell_provider::GenerationContext& ctx) const {
    if (!ctx.ask_user_enabled) return {};

    openai::ToolFunction function;
    function.name = kToolName;
    function.description =
        "Ask the user a question. Use it when the decision is the user's to make and guessing "
        "would waste work. Supply options when the answer is a choice, or leave them out to ask "
        "an open question; the user can always type their own answer instead. Ask one question "
        "with `question`, or put a few related ones in `questions` so they arrive together; "
        "never ask the same thing twice in a row.";

    openai::ToolProperty question;
    question.type = "string";
    question.description =
        "The question, written in the user's language. Use this for a single question, or use "
        "`questions` instead.";

    openai::ToolProperty option_item;
    option_item.type = "string";
    option_item.description = "One selectable answer.";
    openai::ToolProperty options;
    options.type = "array";
    options.description =
        "Optional answers to choose from for `question`. Keep each one short. Omit them for an "
        "open question.";
    options.items = std::make_shared<openai::ToolProperty>(std::move(option_item));

    openai::ToolProperty allow_multiple;
    allow_multiple.type = "boolean";
    allow_multiple.description =
        "True lets the user pick several options for `question`. Defaults to false.";

    openai::ToolProperty inner_question;
    inner_question.type = "string";
    inner_question.description = "The question, written in the user's language.";
    openai::ToolProperty inner_options;
    inner_options.type = "array";
    inner_options.description = "Optional answers to choose from. Keep each one short.";
    openai::ToolProperty inner_option_item;
    inner_option_item.type = "string";
    inner_option_item.description = "One selectable answer.";
    inner_options.items = std::make_shared<openai::ToolProperty>(std::move(inner_option_item));
    openai::ToolProperty inner_allow_multiple;
    inner_allow_multiple.type = "boolean";
    inner_allow_multiple.description =
        "True lets the user pick several of this question's options. Defaults to false.";

    openai::ToolProperty question_item;
    question_item.type = "object";
    question_item.description = "One question of the set.";
    question_item.properties = {
        {"question", std::move(inner_question)},
        {"options", std::move(inner_options)},
        {"allow_multiple", std::move(inner_allow_multiple)},
    };
    question_item.required = {"question"};

    openai::ToolProperty questions;
    questions.type = "array";
    questions.description =
        "Several questions to ask at once, each with its own options. They appear together and "
        "can be answered in any order. Use this instead of `question`, not with it, and keep it "
        "to at most " +
        std::to_string(kMaxQuestions) + " questions.";
    questions.items = std::make_shared<openai::ToolProperty>(std::move(question_item));

    openai::ToolProperty blocking;
    blocking.type = "boolean";
    blocking.description =
        "True waits here until the user answers, so the answer is part of this result. False "
        "returns immediately and you keep working; the answer arrives later as a user message, "
        "so only use it when you have work that does not depend on the answer. Defaults to true.";

    function.parameters.properties = {
        {"question", std::move(question)},
        {"options", std::move(options)},
        {"allow_multiple", std::move(allow_multiple)},
        {"questions", std::move(questions)},
        {"blocking", std::move(blocking)},
    };
    function.parameters.required = {};  // required = emptyList()

    return {openai::ToolDefinition{"function", std::move(function)}};
}

std::string AskUserToolProvider::execute(const std::string& /*name*/,
                                         const std::string& arguments,
                                         const shell_provider::GenerationContext& ctx) {
    // runCatching { parse as JsonObject }.getOrNull() — malformed or non-object → failure.
    const std::string args_text = json::is_blank(arguments) ? "{}" : arguments;
    const json::ParseResult parsed_args = json::parse(args_text);
    if (!parsed_args.value || parsed_args.value->kind != json::Kind::Object) {
        return failure("bad_arguments", "Arguments are not a JSON object.");
    }
    const json::Value& args = *parsed_args.value;

    // `args["blocking"]?.jsonPrimitive?.booleanOrNull ?: true` — jsonPrimitive throws on
    // non-primitives; booleanOrNull is case-sensitive exact "true"/"false".
    bool blocking = true;
    if (const json::Value* element = map_get(args, "blocking"); element != nullptr) {
        const std::optional<std::string> content = primitive_content(*element);
        if (!content.has_value()) throw std::runtime_error("Element is not a JsonPrimitive");
        if (const std::optional<bool> parsed = to_boolean_strict_or_null(*content)) {
            blocking = *parsed;
        }
    }
    if (!blocking && !ctx.conversation_id.has_value()) {
        return failure(
            "no_conversation",
            "A non-blocking answer travels as a user message, which needs a conversation. Ask "
            "with blocking set to true here.");
    }

    const ParsedQuestions parsed_questions = parse_questions(args);
    if (!parsed_questions.valid) return failure(parsed_questions.code, parsed_questions.detail);
    const std::vector<PendingQuestion>& questions = parsed_questions.questions;

    // A set is one card with a page per question, so its questions share one set id.
    std::optional<std::int64_t> set_id;
    if (questions.size() > 1) set_id = host_.new_set_id();

    std::vector<std::int64_t> request_ids;
    request_ids.reserve(questions.size());
    for (const PendingQuestion& question : questions) {
        request_ids.push_back(host_.open(ctx.conversation_id, question.question, question.options,
                                         question.allow_multiple, blocking, set_id));
    }
    if (!blocking) return queued_result(request_ids.size());

    // Whatever is still waiting belongs to a wait that no longer exists — abandoned on both
    // the success and the failure path, exactly like Kotlin's `finally`.
    const auto abandon_all = [&] {
        for (const std::int64_t id : request_ids) host_.abandon(id);
    };

    std::vector<Answer> answers;
    std::string result;
    try {
        answers.reserve(request_ids.size());
        for (const std::int64_t id : request_ids) answers.push_back(host_.await_answer(id));
        result = answers.size() == 1 ? single_result(answers.front())
                                     : group_result(questions, answers);
    } catch (...) {
        abandon_all();
        throw;
    }
    abandon_all();
    return result;
}

}  // namespace agora::core::askuser