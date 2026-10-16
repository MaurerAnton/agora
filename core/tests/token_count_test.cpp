#include <stdexcept>
#include <string_view>

#include "check.hpp"

#include "agora/core/bpe_vocabulary.hpp"
#include "agora/core/text_counter.hpp"
#include "agora/core/token_count.hpp"
#include "agora/core/unicode.hpp"

using agora::core::BpeTextCounter;
using agora::core::BpeVocabulary;
using agora::core::HeuristicTextCounter;

namespace {

/** Single letters plus the pair "he", so "hello" must merge into he-l-l-o. */
BpeVocabulary tiny_vocabulary() {
    return BpeVocabulary::parse_tiktoken(
        "aA== 0\n"    // h
        "ZQ== 1\n"    // e
        "bA== 2\n"    // l
        "bw== 3\n"    // o
        "aGU= 4\n");  // he
}

BpeVocabulary whole_word_vocabulary() {
    return BpeVocabulary::parse_tiktoken(
        "aA== 0\n"
        "ZQ== 1\n"
        "bA== 2\n"
        "bw== 3\n"
        "aGVsbG8= 4\n");  // hello
}

}  // namespace

TEST(tiktoken_parsing_skips_malformed_lines_and_keeps_the_rest) {
    const BpeVocabulary vocabulary = BpeVocabulary::parse_tiktoken(
        "\n"
        "   \n"
        "no-space-here\n"
        " QQ== 1\n"           // separator at position zero
        "QQ== not-a-number\n" // rank is not an integer
        "QQ=A 2\n"            // '=' inside the base64 payload
        "QQ== 1\n"            // 'A'
        "Yg== 6\r\n"          // 'b', CRLF terminator
        "ICA= 5\n");          // two spaces
    CHECK_EQ(std::size_t(3), vocabulary.size());
    CHECK_EQ(1, vocabulary.rank_of("A"));
    CHECK_EQ(6, vocabulary.rank_of("b"));
    CHECK_EQ(5, vocabulary.rank_of("  "));
    CHECK_EQ(BpeVocabulary::kAbsent, vocabulary.rank_of("B"));
}

TEST(a_lone_continuation_byte_is_never_a_token) {
    const BpeVocabulary vocabulary = tiny_vocabulary();
    const auto orphan = "\x80\x80\x80\x80";
    CHECK_EQ(BpeVocabulary::kAbsent, vocabulary.rank_of(std::string_view(orphan, 4)));
    CHECK_EQ(4, vocabulary.rank_of("he"));
}

TEST(text_is_merged_by_rank_until_no_pair_is_left) {
    const BpeVocabulary vocabulary = tiny_vocabulary();
    const BpeTextCounter counter(&vocabulary, 1.0, &HeuristicTextCounter::instance());
    // h e l l o merges to he l l o: four parts, because no other pair is in the vocabulary.
    CHECK_EQ(std::int64_t(4), counter.count("hello"));
}

TEST(a_piece_that_is_itself_a_token_costs_one) {
    const BpeVocabulary vocabulary = whole_word_vocabulary();
    const BpeTextCounter counter(&vocabulary, 1.0, &HeuristicTextCounter::instance());
    CHECK_EQ(std::int64_t(1), counter.count("hello"));
}

TEST(a_vocabulary_that_has_not_loaded_falls_back_to_the_heuristic) {
    const BpeTextCounter counter(nullptr, 1.0, &HeuristicTextCounter::instance());
    const std::string_view text = "The quick brown fox jumps over the lazy dog.";
    CHECK_EQ(HeuristicTextCounter::instance().count(text), counter.count(text));
}

TEST(empty_text_costs_nothing_either_way) {
    const BpeVocabulary vocabulary = tiny_vocabulary();
    const BpeTextCounter loaded(&vocabulary, 1.0, &HeuristicTextCounter::instance());
    const BpeTextCounter unloaded(nullptr, 1.0, &HeuristicTextCounter::instance());
    CHECK_EQ(std::int64_t(0), loaded.count(""));
    CHECK_EQ(std::int64_t(0), unloaded.count(""));
}

TEST(a_family_factor_scales_the_count_upwards_and_never_fractionally) {
    const BpeVocabulary vocabulary = tiny_vocabulary();
    const BpeTextCounter plain(&vocabulary, 1.0, &HeuristicTextCounter::instance());
    const BpeTextCounter scaled(&vocabulary, 1.15, &HeuristicTextCounter::instance());
    // 4 * 1.15 is 4.6, which has to become 5 rather than 4.
    CHECK_EQ(std::int64_t(4), plain.count("hello"));
    CHECK_EQ(std::int64_t(5), scaled.count("hello"));
}

TEST(a_factor_below_one_is_refused) {
    const BpeVocabulary vocabulary = tiny_vocabulary();
    bool refused = false;
    try {
        const BpeTextCounter counter(&vocabulary, 0.9, &HeuristicTextCounter::instance());
        (void)counter;
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CHECK(refused);
}

TEST(the_memo_answers_identically_on_first_and_repeated_lookups) {
    const BpeVocabulary vocabulary = tiny_vocabulary();
    const BpeTextCounter counter(&vocabulary, 1.0, &HeuristicTextCounter::instance());
    const std::string text = [] {
        std::string built;
        for (int index = 0; index < 40; ++index) built += "hello ";
        return built;
    }();
    const std::int64_t first = counter.count(text);
    CHECK_EQ(first, counter.count(text));
    CHECK_EQ(first, counter.count(std::string(text)));
    CHECK_EQ(first, agora::core::bpe_token_count(text, vocabulary));
}

TEST(a_long_ascii_run_is_merged_in_chunks) {
    // "a" and "aa" only: the whole run halves until pairs stop existing. The run crosses the
    // 512-byte chunk boundary, so the two chunks count as 256 + 44.
    const BpeVocabulary vocabulary = BpeVocabulary::parse_tiktoken(
        "YQ== 0\n"     // a
        "YWE= 1\n");   // aa
    const BpeTextCounter counter(&vocabulary, 1.0, &HeuristicTextCounter::instance());
    const std::string text(600, 'a');
    CHECK_EQ(std::int64_t(300), counter.count(text));
}

TEST(heuristic_shapes_match_the_kotlin_definition) {
    const HeuristicTextCounter& heuristic = HeuristicTextCounter::instance();
    CHECK_EQ(std::int64_t(0), heuristic.count(""));
    CHECK_EQ(std::int64_t(0), heuristic.count("\t \n"));
    // U+00A0 is a character Java calls a space, not whitespace: it costs a token.
    CHECK_EQ(std::int64_t(1), heuristic.count("\u00A0"));
    CHECK_EQ(std::int64_t(1), heuristic.count("abcd"));
    CHECK_EQ(std::int64_t(2), heuristic.count("abcde"));
    CHECK_EQ(std::int64_t(1), heuristic.count("\u4E2D"));      // 中
    CHECK_EQ(std::int64_t(2), heuristic.count("a\u4E2D"));
    CHECK_EQ(std::int64_t(3), heuristic.count("!!!"));
    CHECK_EQ(std::int64_t(2), heuristic.count("a b"));
    CHECK_EQ(std::int64_t(2), heuristic.count("\u041F\u0440"));  // П р: each non-ASCII letter costs one
}

TEST(unicode_classes_match_the_jvm) {
    // Spot checks against the JVM tables this port is generated from.
    CHECK(agora::core::unicode::is_letter(U'\u4E2D'));
    CHECK(agora::core::unicode::is_number(U'7'));
    CHECK(agora::core::unicode::is_jvm_whitespace(U'\u2003'));
    CHECK(!agora::core::unicode::is_jvm_whitespace(U'\u00A0'));
    CHECK(agora::core::unicode::is_kotlin_whitespace(U'\u00A0'));
    CHECK(!agora::core::unicode::is_letter(U'\u3000'));
}
