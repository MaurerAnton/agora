// Unit pins for the message-preparation pieces the golden scenarios cannot reach: the split
// functions' require() gate, the heuristic counter's character classes, the per-family image
// rules on direct pixel vectors, model-id parsing and the safety-margin rounding. Everything
// reachable through conversations is pinned by message_preparation_parity_test.cpp.
#include <cstdint>
#include <string>
#include <vector>

#include "agora/core/message_preparation.hpp"
#include "check.hpp"

namespace {

namespace prep = agora::core::prep;

prep::ChatMessage user_row(std::string id, std::string text) {
    prep::ChatMessage message;
    message.id = std::move(id);
    message.text = std::move(text);
    message.participant = prep::Participant::User;
    return message;
}

}  // namespace

TEST(prep_split_requires_non_negative_retention) {
    const std::vector<prep::ChatMessage> messages{user_row("user_1", "hi")};
    bool threw = false;
    try {
        (void)prep::split_context_for_compact_retention(messages, -1);
    } catch (const prep::PrepError& error) {
        threw = true;
        CHECK_EQ(std::string("IllegalArgumentException"), error.class_name);
    }
    CHECK(threw);
    threw = false;
    try {
        (void)prep::split_logical_context(messages, -2);
    } catch (const prep::PrepError& error) {
        threw = true;
        CHECK_EQ(std::string("IllegalArgumentException"), error.class_name);
    }
    CHECK(threw);
}

TEST(prep_heuristic_counter_classes) {
    // ASCII word-like runs: four characters per token, rounded up; whitespace only terminates.
    CHECK_EQ(std::int64_t{0}, prep::heuristic_count(""));
    CHECK_EQ(std::int64_t{1}, prep::heuristic_count("abcd"));
    CHECK_EQ(std::int64_t{2}, prep::heuristic_count("abcde"));
    CHECK_EQ(std::int64_t{2}, prep::heuristic_count("ab cd"));
    // Every non-run code point costs one token: CJK letters are not ASCII runs.
    CHECK_EQ(std::int64_t{2}, prep::heuristic_count("图像"));
    // Punctuation flushes and charges one token of its own; a tab only terminates a run.
    CHECK_EQ(std::int64_t{2}, prep::heuristic_count("a\tb"));
    CHECK_EQ(std::int64_t{3}, prep::heuristic_count("a,b"));
}

TEST(prep_safety_margin_rounds_up) {
    const prep::SafetyMargin margin;
    CHECK_EQ(0, margin.apply(0));    // (0 * 11 + 9) / 10 == 0
    CHECK_EQ(2, margin.apply(1));    // 1.1 rounds up to 2
    CHECK_EQ(11, margin.apply(10));  // exactly 10 * 1.1
    CHECK_EQ(13, margin.apply(11));  // 12.1 rounds up to 13
}

TEST(prep_image_rules_by_pixels) {
    const prep::ImageDescriptor pixels{std::nullopt, 1024, 768};
    const prep::ImageDescriptor bytes{768000, std::nullopt, std::nullopt};
    const prep::ImageDescriptor unknown;

    // Byte proportional: 768 KiB -> 1000 tokens inside the [256, 1536] window; unknown -> 1024.
    CHECK_EQ(std::int64_t{1000}, prep::byte_proportional_image_tokens(bytes));
    CHECK_EQ(std::int64_t{1024}, prep::byte_proportional_image_tokens(unknown));

    // OpenAI tile rule (gpt-4.1 family): short side scaled to 768, 512px tiles, +85 base.
    CHECK_EQ(std::int64_t{85 + 2 * 2 * 170}, prep::openai_tile_image_tokens(1024, 768, 85, 170));

    // Gemini: below 384px a flat 258; the 960x540 guide example is 3x2 tiles.
    CHECK_EQ(std::int64_t{258}, prep::gemini_image_tokens(prep::ImageDescriptor{std::nullopt, 300, 200}));
    CHECK_EQ(std::int64_t{6 * 258}, prep::gemini_image_tokens(prep::ImageDescriptor{std::nullopt, 960, 540}));

    // Anthropic standard tier: bisection keeps the visual tokens inside 1568 for any size.
    CHECK(prep::anthropic_image_tokens(prep::ImageDescriptor{std::nullopt, 4000, 3000}, false) <= 1568);
    CHECK(prep::anthropic_image_tokens(prep::ImageDescriptor{std::nullopt, 4000, 3000}, true) <= 4784);
    // A small image costs its exact patch grid.
    CHECK_EQ(std::int64_t{2 * 2}, prep::anthropic_image_tokens(prep::ImageDescriptor{std::nullopt, 56, 40}, false));
}

TEST(prep_prefixed_model_id_parsing) {
    const auto prefixed = prep::parse_prefixed_model_id("OpenAI:gpt-4.1");
    CHECK_EQ(std::string("OpenAI"), prefixed.provider_name);
    CHECK_EQ(std::string("gpt-4.1"), prefixed.model_name);
    // Legacy unprefixed ids use the documented heuristics.
    CHECK_EQ(std::string("Anthropic"), prep::parse_prefixed_model_id("claude-3-opus").provider_name);
    CHECK_EQ(std::string("DeepSeek"), prep::parse_prefixed_model_id("models/deepseek-v3").provider_name);
    CHECK_EQ(std::string("Qwen"), prep::parse_prefixed_model_id("qwen3-30b-a3b").provider_name);
    CHECK_EQ(std::string("Unknown"), prep::parse_prefixed_model_id("mystery-model").provider_name);
    // A provider name decides the family even for an unusual model name.
    CHECK(prep::family_of(prep::PrefixedModelId{"openai", "custom-relay"}) ==
          prep::ContextCostFamily::OpenAi);
    CHECK(prep::family_of(std::nullopt) == prep::ContextCostFamily::Generic);
}
