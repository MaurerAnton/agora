#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"

#include "agora/core/bpe_vocabulary.hpp"
#include "agora/core/token_count.hpp"

#ifndef AGORA_FIXTURE_CORPUS
#error "AGORA_FIXTURE_CORPUS must point at core/tools/golden/corpus.b64"
#endif
#ifndef AGORA_FIXTURE_CORPUS_COUNTS
#error "AGORA_FIXTURE_CORPUS_COUNTS must point at core/tools/golden/golden_counts.txt"
#endif

namespace {

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(std::string("cannot open ") + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> read_lines(const char* path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error(std::string("cannot open ") + path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

int base64_value(char symbol) {
    if (symbol >= 'A' && symbol <= 'Z') return symbol - 'A';
    if (symbol >= 'a' && symbol <= 'z') return symbol - 'a' + 26;
    if (symbol >= '0' && symbol <= '9') return symbol - '0' + 52;
    if (symbol == '+') return 62;
    if (symbol == '/') return 63;
    return -1;
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0 && text[end - 1] == '=') end--;
    std::vector<std::uint8_t> out;
    out.reserve(end * 3 / 4);
    int accumulator = 0;
    int bits_held = 0;
    for (std::size_t at = 0; at < end; ++at) {
        const int value = base64_value(text[at]);
        if (value < 0) throw std::runtime_error("bad base64 in corpus");
        accumulator = (accumulator << 6) | value;
        bits_held += 6;
        if (bits_held >= 8) {
            bits_held -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits_held) & 0xFF));
        }
    }
    return out;
}

/** A short, readable form of a case for the failure message. */
std::string preview(const std::string& bytes) {
    std::string out;
    for (const char symbol : bytes) {
        const auto byte = static_cast<unsigned char>(symbol);
        if (byte >= 0x20 && byte < 0x7F) {
            out.push_back(symbol);
        } else {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\x%02X", byte);
            out += hex;
        }
        if (out.size() > 90) {
            out += "...";
            break;
        }
    }
    return out;
}

}  // namespace

/**
 * The differentiation that pinned this port to the Kotlin implementation, frozen as a test.
 *
 * core/tools/golden/corpus.b64 holds base64-encoded texts (real repository text plus structured
 * fuzz over letters, marks, digits, whitespace, contractions and punctuation);
 * golden_counts.txt holds the counts the Kotlin implementation produced for them. Regenerate with
 * core/tools/golden/check_parity.sh when the corpus is extended.
 */
TEST(the_port_counts_like_kotlin_on_the_frozen_corpus) {
    const std::string vocabulary_content = read_file(AGORA_FIXTURE_VOCABULARY);
    const agora::core::BpeVocabulary vocabulary =
        agora::core::BpeVocabulary::parse_tiktoken(vocabulary_content);
    const std::vector<std::string> corpus = read_lines(AGORA_FIXTURE_CORPUS);
    const std::vector<std::string> golden = read_lines(AGORA_FIXTURE_CORPUS_COUNTS);
    CHECK_EQ(corpus.size(), golden.size());
    CHECK(corpus.size() >= 2000);

    for (std::size_t index = 0; index < corpus.size(); ++index) {
        const std::vector<std::uint8_t> bytes = base64_decode(corpus[index]);
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::int64_t counted = agora::core::bpe_token_count(text, vocabulary);
        const std::int64_t expected = std::stoll(golden[index]);
        if (counted != expected) {
            agora::core::test::fail(
                __FILE__,
                __LINE__,
                "case " + std::to_string(index) + ": kotlin counted " +
                    std::to_string(expected) + " but the port counted " +
                    std::to_string(counted) + " for: " + preview(std::string(text))
            );
        }
    }
}
