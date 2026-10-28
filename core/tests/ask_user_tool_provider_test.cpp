// Unit tests for the AskUserToolProvider port: the nested tool schema, the failure envelopes,
// the blocking/question parsing quirks and the Host (AskUserController) interaction order.

#include "agora/core/ask_user_tool_provider.hpp"

#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"

namespace askuser = agora::core::askuser;
namespace shell_provider = agora::core::shell_provider;
using askuser::AskUserToolProvider;

namespace {

struct ScriptedHost final : askuser::Host {
    struct OpenCall {
        std::optional<std::string> conversation_id;
        std::string question;
        std::vector<std::string> options;
        bool allow_multiple = false;
        bool blocking = false;
        std::optional<std::int64_t> set_id;
    };

    int new_set_id_calls = 0;
    std::int64_t next_set_id = 100;
    std::int64_t next_request_id = 1;
    std::vector<OpenCall> opens;
    std::vector<std::int64_t> awaited;
    std::vector<std::int64_t> abandoned;
    std::deque<askuser::Answer> answers;  // consumed per await_answer, in order
    bool throw_on_await = false;

    std::int64_t new_set_id() override {
        ++new_set_id_calls;
        return next_set_id++;
    }

    std::int64_t open(const std::optional<std::string>& conversation_id,
                      const std::string& question, const std::vector<std::string>& options,
                      bool allow_multiple, bool blocking,
                      const std::optional<std::int64_t>& set_id) override {
        opens.push_back(OpenCall{conversation_id, question, options, allow_multiple, blocking,
                                 set_id});
        return next_request_id++;
    }

    askuser::Answer await_answer(std::int64_t request_id) override {
        awaited.push_back(request_id);
        if (throw_on_await) throw std::runtime_error("waiter cancelled");
        if (answers.empty()) return askuser::Answer::unanswered();
        const askuser::Answer answer = answers.front();
        answers.pop_front();
        return answer;
    }

    void abandon(std::int64_t request_id) override { abandoned.push_back(request_id); }

    void answer_with(std::vector<std::string> choices, std::optional<std::string> text = std::nullopt) {
        answers.push_back(askuser::Answer{std::move(choices), std::move(text), true});
    }
};

struct Fixture {
    ScriptedHost host;
    AskUserToolProvider provider;

    Fixture() : provider(host) {}
};

shell_provider::GenerationContext make_ctx() {
    shell_provider::GenerationContext ctx;
    ctx.ask_user_enabled = true;
    return ctx;
}

}  // namespace

// ---- Definitions ---------------------------------------------------------------------------------

TEST(ask_user_definitions_gated_by_context) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.ask_user_enabled = false;
    CHECK(fixture.provider.definitions(ctx).empty());
    // handles() is independent of the gate.
    CHECK(fixture.provider.handles("ask_user"));
    CHECK(!fixture.provider.handles("web_search"));
    CHECK(!fixture.provider.handles("execute_shell_command"));
}

TEST(ask_user_definitions_exact_shapes) {
    Fixture fixture;
    const auto definitions = fixture.provider.definitions(make_ctx());
    CHECK_EQ(std::size_t(1), definitions.size());

    const auto& function = definitions[0].function;
    CHECK_EQ(std::string("function"), definitions[0].type);
    CHECK_EQ(std::string("ask_user"), function.name);
    CHECK_EQ(
        std::string("Ask the user a question. Use it when the decision is the user's to make "
                    "and guessing would waste work. Supply options when the answer is a choice, "
                    "or leave them out to ask an open question; the user can always type their "
                    "own answer instead. Ask one question with `question`, or put a few related "
                    "ones in `questions` so they arrive together; never ask the same thing twice "
                    "in a row."),
        function.description);
    CHECK(function.parameters.required.empty());  // required = emptyList()

    const auto& properties = function.parameters.properties;
    CHECK_EQ(std::size_t(5), properties.size());
    CHECK_EQ(std::string("question"), properties[0].first);
    CHECK_EQ(std::string("string"), properties[0].second.type);
    CHECK_EQ(
        std::string("The question, written in the user's language. Use this for a single "
                    "question, or use `questions` instead."),
        properties[0].second.description);
    CHECK(!properties[0].second.items);

    CHECK_EQ(std::string("options"), properties[1].first);
    CHECK_EQ(std::string("array"), properties[1].second.type);
    CHECK_EQ(
        std::string("Optional answers to choose from for `question`. Keep each one short. "
                    "Omit them for an open question."),
        properties[1].second.description);
    CHECK(properties[1].second.items);
    CHECK_EQ(std::string("string"), properties[1].second.items->type);
    CHECK_EQ(std::string("One selectable answer."), properties[1].second.items->description);

    CHECK_EQ(std::string("allow_multiple"), properties[2].first);
    CHECK_EQ(std::string("boolean"), properties[2].second.type);
    CHECK_EQ(std::string("True lets the user pick several options for `question`. Defaults to "
                         "false."),
             properties[2].second.description);

    CHECK_EQ(std::string("questions"), properties[3].first);
    CHECK_EQ(std::string("array"), properties[3].second.type);
    CHECK_EQ(
        std::string("Several questions to ask at once, each with its own options. They appear "
                    "together and can be answered in any order. Use this instead of `question`, "
                    "not with it, and keep it to at most 5 questions."),
        properties[3].second.description);
    CHECK(properties[3].second.items);
    const auto& item = *properties[3].second.items;
    CHECK_EQ(std::string("object"), item.type);
    CHECK_EQ(std::string("One question of the set."), item.description);
    CHECK(item.required);
    CHECK_EQ(std::size_t(1), item.required->size());
    CHECK_EQ(std::string("question"), item.required->front());
    CHECK(item.properties);
    CHECK_EQ(std::size_t(3), item.properties->size());
    CHECK_EQ(std::string("question"), (*item.properties)[0].first);
    CHECK_EQ(std::string("string"), (*item.properties)[0].second.type);
    CHECK_EQ(std::string("The question, written in the user's language."),
             (*item.properties)[0].second.description);
    CHECK_EQ(std::string("options"), (*item.properties)[1].first);
    CHECK_EQ(std::string("array"), (*item.properties)[1].second.type);
    CHECK_EQ(std::string("Optional answers to choose from. Keep each one short."),
             (*item.properties)[1].second.description);
    CHECK((*item.properties)[1].second.items);
    CHECK_EQ(std::string("string"), (*item.properties)[1].second.items->type);
    CHECK_EQ(std::string("One selectable answer."),
             (*item.properties)[1].second.items->description);
    CHECK_EQ(std::string("allow_multiple"), (*item.properties)[2].first);
    CHECK_EQ(std::string("boolean"), (*item.properties)[2].second.type);
    CHECK_EQ(std::string("True lets the user pick several of this question's options. "
                         "Defaults to false."),
             (*item.properties)[2].second.description);

    CHECK_EQ(std::string("blocking"), properties[4].first);
    CHECK_EQ(std::string("boolean"), properties[4].second.type);
    CHECK_EQ(
        std::string("True waits here until the user answers, so the answer is part of this "
                    "result. False returns immediately and you keep working; the answer arrives "
                    "later as a user message, so only use it when you have work that does not "
                    "depend on the answer. Defaults to true."),
        properties[4].second.description);
}

// ---- Argument parsing failures ---------------------------------------------------------------------

TEST(ask_user_bad_arguments_envelope) {
    Fixture fixture;
    const auto ctx = make_ctx();
    // Non-object top-level and malformed JSON both fold into bad_arguments (runCatching).
    const std::string expected =
        R"({"error":"bad_arguments","detail":"Arguments are not a JSON object."})";
    CHECK_EQ(expected, fixture.provider.execute("ask_user", "[1,2]", ctx));
    CHECK_EQ(expected, fixture.provider.execute("ask_user", "{oops", ctx));
    CHECK_EQ(expected, fixture.provider.execute("ask_user", "\"just a string\"", ctx));
    // Blank arguments become "{}" (Kotlin ifBlank, whitespace counts) → no_question instead.
    CHECK_EQ(std::string(R"({"error":"no_question","detail":"A question is required."})"),
             fixture.provider.execute("ask_user", "", ctx));
    CHECK_EQ(std::string(R"({"error":"no_question","detail":"A question is required."})"),
             fixture.provider.execute("ask_user", "   ", ctx));
    // The failure path never touches the host.
    CHECK(fixture.host.opens.empty());
}

TEST(ask_user_question_form_failures) {
    Fixture fixture;
    const auto ctx = make_ctx();
    // Neither form.
    CHECK_EQ(std::string(R"({"error":"no_question","detail":"A question is required."})"),
             fixture.provider.execute("ask_user", R"({"options":["a"]})", ctx));
    // Both forms together are refused (the set form and the single form describe one thing).
    // Expected envelopes use one terminated raw literal per line (adjacent literals concatenate
    // without the line break — a raw string would swallow it as content).
    CHECK_EQ(std::string(
                 R"({"error":"ambiguous_questions","detail":"Use question for one )"
                 R"(question or questions for a set, not both."})"),
             fixture.provider.execute(
                 "ask_user", R"({"question":"Q","questions":[{"question":"Q2"}]})", ctx));
    // More than MAX_QUESTIONS=5 in the set form.
    CHECK_EQ(std::string(
                 R"({"error":"too_many_questions","detail":"Ask at most 5 questions in )"
                 R"(one call."})"),
             fixture.provider.execute(
                 "ask_user",
                 R"({"questions":[{"question":"1"},{"question":"2"},{"question":"3"},)"
                 R"({"question":"4"},{"question":"5"},{"question":"6"}]})",
                 ctx));
    // A non-object entry, then an entry without a question field.
    CHECK_EQ(std::string(
                 R"({"error":"bad_question","detail":"Each entry of questions must be an )"
                 R"(object with a question field."})"),
             fixture.provider.execute("ask_user", R"({"questions":["nope"]})", ctx));
    CHECK_EQ(std::string(
                 R"({"error":"no_question","detail":"Each entry of questions needs a )"
                 R"(question field."})"),
             fixture.provider.execute("ask_user", R"({"questions":[{"options":["a"]}]})", ctx));
    // A whitespace-only question is blank → treated as absent per stringOrNull.
    CHECK_EQ(std::string(R"({"error":"no_question","detail":"A question is required."})"),
             fixture.provider.execute("ask_user", R"({"question":"   "})", ctx));
    CHECK(fixture.host.opens.empty());
}

// ---- blocking / allow_multiple parsing (kotlinx quirk pinning) ------------------------------------

TEST(ask_user_blocking_and_multiple_parse) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.conversation_id = "conv-7";
    fixture.host.answer_with({"yes"});

    // Absent → blocking default true.
    fixture.provider.execute("ask_user", R"({"question":"Q"})", ctx);
    CHECK(fixture.host.opens.back().blocking);
    // Exact "true"/"false" (case-sensitive, string or literal).
    fixture.provider.execute("ask_user", R"({"question":"Q","blocking":false})", ctx);
    CHECK(!fixture.host.opens.back().blocking);
    fixture.provider.execute("ask_user", R"({"question":"Q","blocking":"false"})", ctx);
    CHECK(!fixture.host.opens.back().blocking);
    fixture.provider.execute("ask_user", R"({"question":"Q","blocking":"TRUE"})", ctx);
    CHECK(fixture.host.opens.back().blocking);  // toBooleanStrictOrNull → null → default
    fixture.provider.execute("ask_user", R"({"question":"Q","blocking":1})", ctx);
    CHECK(fixture.host.opens.back().blocking);
    fixture.provider.execute("ask_user", R"({"question":"Q","blocking":null})", ctx);
    CHECK(fixture.host.opens.back().blocking);

    // Non-primitive jsonPrimitive throws (kotlinx accessor contract).
    bool threw = false;
    try {
        fixture.provider.execute("ask_user", R"({"question":"Q","blocking":{}})", ctx);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // allow_multiple: content-based toBooleanStrictOrNull, default false.
    fixture.provider.execute("ask_user", R"({"question":"Q","allow_multiple":"true"})", ctx);
    CHECK(fixture.host.opens.back().allow_multiple);
    fixture.provider.execute("ask_user", R"({"question":"Q","allow_multiple":true})", ctx);
    CHECK(fixture.host.opens.back().allow_multiple);
    fixture.provider.execute("ask_user", R"({"question":"Q","allow_multiple":"TRUE"})", ctx);
    CHECK(!fixture.host.opens.back().allow_multiple);  // case-sensitive → null → false
    fixture.provider.execute("ask_user", R"({"question":"Q","allow_multiple":1})", ctx);
    CHECK(!fixture.host.opens.back().allow_multiple);
    fixture.provider.execute("ask_user", R"({"question":"Q","allow_multiple":null})", ctx);
    CHECK(!fixture.host.opens.back().allow_multiple);
    fixture.provider.execute("ask_user", R"({"question":"Q"})", ctx);
    CHECK(!fixture.host.opens.back().allow_multiple);

    threw = false;
    try {
        fixture.provider.execute("ask_user", R"({"question":"Q","allow_multiple":[]})", ctx);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(ask_user_requires_conversation_when_non_blocking) {
    Fixture fixture;
    auto ctx = make_ctx();  // conversation_id unset
    const std::string result = fixture.provider.execute(
        "ask_user", R"({"question":"Q","blocking":false})", ctx);
    CHECK_EQ(
        std::string(
            R"({"error":"no_conversation","detail":"A non-blocking answer travels as a user message, )")
                .append(R"(which needs a conversation. Ask with blocking set to true here."})"),
        result);
    CHECK(fixture.host.opens.empty());
}

// ---- Options / questions extraction ----------------------------------------------------------------------------------

TEST(ask_user_options_extraction) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.host.answer_with({"a"});

    // Primitives keep their content (numbers stringify), blanks drop, JsonNull content
    // "null" is kept (it IS a JsonPrimitive), duplicates collapse to first occurrence,
    // objects are dropped by the `as? JsonPrimitive` cast.
    fixture.provider.execute(
        "ask_user",
        R"({"question":"Q","options":["a","",5,"a"," ","null",{"x":1}]})",
        ctx);
    const auto& call = fixture.host.opens.back();
    CHECK_EQ(std::size_t(3), call.options.size());
    CHECK_EQ(std::string("a"), call.options[0]);
    CHECK_EQ(std::string("5"), call.options[1]);
    CHECK_EQ(std::string("null"), call.options[2]);

    // A non-array options field is treated as absent.
    fixture.provider.execute("ask_user", R"({"question":"Q","options":"nope"})", ctx);
    CHECK(fixture.host.opens.back().options.empty());

    // The JsonNull question IS a primitive: content "null" reaches the bar (kotlinx quirk).
    fixture.provider.execute("ask_user", R"({"question":null})", ctx);
    CHECK_EQ(std::string("null"), fixture.host.opens.back().question);
}

// ---- Results and the Host interaction order ------------------------------------------------------------------------------

TEST(ask_user_single_blocking_result) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.conversation_id = "conv-1";
    fixture.host.answer_with({"yes", "no"}, "typed answer");

    const std::string result = fixture.provider.execute(
        "ask_user",
        R"({"question":"Ship it?","options":["yes","no"],"allow_multiple":true})", ctx);
    CHECK_EQ(std::string(R"({"answered":true,"choices":["yes","no"],"text":"typed answer"})"),
             result);

    CHECK_EQ(std::size_t(1), fixture.host.opens.size());
    const auto& call = fixture.host.opens.front();
    CHECK(call.conversation_id.has_value());
    CHECK_EQ(std::string("conv-1"), *call.conversation_id);
    CHECK_EQ(std::string("Ship it?"), call.question);
    CHECK_EQ(std::size_t(2), call.options.size());
    CHECK(call.allow_multiple);
    CHECK(call.blocking);
    CHECK(!call.set_id.has_value());  // single questions carry no set id
    // await for the request, then abandoned by the finally (one id, in order).
    CHECK_EQ(std::size_t(1), fixture.host.awaited.size());
    CHECK_EQ(std::size_t(1), fixture.host.abandoned.size());
    CHECK_EQ(fixture.host.awaited[0], fixture.host.abandoned[0]);
}

TEST(ask_user_answer_shapes) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.conversation_id = "c";

    // Answered without typed text → no "text" member.
    fixture.host.answer_with({"only-choice"});
    CHECK_EQ(std::string(R"({"answered":true,"choices":["only-choice"]})"),
             fixture.provider.execute("ask_user", R"({"question":"Q"})", ctx));

    // Unanswered (the waiter was missing) → the refusal detail, no choices.
    CHECK_EQ(std::string(
                 R"({"answered":false,"detail":"The user did not answer. Do not invent an )")
                 .append(R"(answer; ask again or continue without it."})"),
             fixture.provider.execute("ask_user", R"({"question":"Q"})", ctx));

    // Choices re-encode as JSON strings even when the model sent numbers.
    fixture.host.answer_with({"7"});
    CHECK_EQ(std::string(R"({"answered":true,"choices":["7"]})"),
             fixture.provider.execute("ask_user", R"({"question":"Q"})", ctx));
}

TEST(ask_user_set_form_shares_set_id_and_groups_results) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.conversation_id = "conv-9";
    fixture.host.answer_with({"opt-a"});
    fixture.host.answers.push_back(askuser::Answer::unanswered());

    const std::string result = fixture.provider.execute(
        "ask_user",
        R"({"questions":[{"question":"First?","options":["opt-a"]},)"
        R"({"question":"Second?"}]})",
        ctx);
    const std::string unanswered_detail =
        "The user did not answer. Do not invent an answer; ask again or continue without it.";
    const std::string expected =
        std::string(R"({"answers":[{"question":"First?","answered":true,"choices":["opt-a"]},)")
            .append(R"({"question":"Second?","answered":false,"detail":")")
            .append(unanswered_detail)
            .append("\"}]}");
    CHECK_EQ(expected, result);

    CHECK_EQ(1, fixture.host.new_set_id_calls);
    CHECK_EQ(std::size_t(2), fixture.host.opens.size());
    CHECK(fixture.host.opens[0].set_id.has_value());
    CHECK(fixture.host.opens[1].set_id.has_value());
    CHECK_EQ(*fixture.host.opens[0].set_id, *fixture.host.opens[1].set_id);
    CHECK_EQ(std::size_t(2), fixture.host.awaited.size());
    CHECK_EQ(std::size_t(2), fixture.host.abandoned.size());
    for (std::size_t i = 0; i < fixture.host.awaited.size(); ++i) {
        CHECK_EQ(fixture.host.awaited[i], fixture.host.abandoned[i]);
    }
}

TEST(ask_user_non_blocking_queues_without_awaiting) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.conversation_id = "conv-2";
    fixture.host.answer_with({"unused"});

    const std::string result = fixture.provider.execute(
        "ask_user", R"({"questions":[{"question":"A"},{"question":"B"}],"blocking":false})", ctx);
    CHECK_EQ(
        std::string(
            R"({"delivery":"queued","questions":2,"detail":"The question is on screen. Keep )")
            .append(R"(working; the user's answer will arrive as a user message. Do not guess it )")
            .append(R"(in the meantime."})"),
        result);
    CHECK_EQ(std::size_t(2), fixture.host.opens.size());
    CHECK(!fixture.host.opens[0].blocking);
    // No waiting, no abandoning: the queued answers travel as user messages later.
    CHECK(fixture.host.awaited.empty());
    CHECK(fixture.host.abandoned.empty());
    // ...and the queued path is what a missing conversation refuses (see the no_conversation test).
}

TEST(ask_user_abandons_all_requests_when_await_throws) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.conversation_id = "conv-3";
    fixture.host.throw_on_await = true;

    bool threw = false;
    try {
        fixture.provider.execute(
            "ask_user",
            R"({"questions":[{"question":"A"},{"question":"B"},{"question":"C"}]})",
            ctx);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    // The first await throws, so only it is recorded; the finally still abandons every
    // request the bar may still hold (all three were opened before the waiting began).
    CHECK_EQ(std::size_t(1), fixture.host.awaited.size());
    CHECK_EQ(std::size_t(3), fixture.host.abandoned.size());
}