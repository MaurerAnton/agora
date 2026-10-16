// The C++ half of the parity stand: reads a corpus of base64-encoded texts and prints, for each,
// either its token count or its pieces (base64), the same shapes the Kotlin GoldenDump prints.
// Used by the locally-run differentiation; core/tools/golden/check_parity.sh drives both sides.
//
// Usage: agora_core_dump <o200k_base.tiktoken> <count|pieces> <corpus.b64>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/bpe_vocabulary.hpp"
#include "agora/core/token_count.hpp"

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
        if (value < 0) {
            std::cerr << "bad base64 character in corpus\n";
            std::exit(2);
        }
        accumulator = (accumulator << 6) | value;
        bits_held += 6;
        if (bits_held >= 8) {
            bits_held -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits_held) & 0xFF));
        }
    }
    return out;
}

std::string base64_encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t at = 0;
    while (at + 3 <= bytes.size()) {
        const std::uint32_t group = (std::uint8_t(bytes[at]) << 16) |
                                    (std::uint8_t(bytes[at + 1]) << 8) |
                                    std::uint8_t(bytes[at + 2]);
        out.push_back(kAlphabet[(group >> 18) & 0x3F]);
        out.push_back(kAlphabet[(group >> 12) & 0x3F]);
        out.push_back(kAlphabet[(group >> 6) & 0x3F]);
        out.push_back(kAlphabet[group & 0x3F]);
        at += 3;
    }
    const std::size_t rest = bytes.size() - at;
    if (rest == 1) {
        const std::uint32_t group = std::uint8_t(bytes[at]) << 16;
        out.push_back(kAlphabet[(group >> 18) & 0x3F]);
        out.push_back(kAlphabet[(group >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const std::uint32_t group = (std::uint8_t(bytes[at]) << 16) |
                                    (std::uint8_t(bytes[at + 1]) << 8);
        out.push_back(kAlphabet[(group >> 18) & 0x3F]);
        out.push_back(kAlphabet[(group >> 12) & 0x3F]);
        out.push_back(kAlphabet[(group >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: agora_core_dump <o200k_base.tiktoken> <count|pieces> <corpus.b64>\n";
        return 2;
    }
    const std::string mode = argv[2];
    if (mode != "count" && mode != "pieces") {
        std::cerr << "mode must be count or pieces\n";
        return 2;
    }

    std::ifstream vocabulary_file(argv[1], std::ios::binary);
    if (!vocabulary_file) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }
    std::ostringstream vocabulary_text;
    vocabulary_text << vocabulary_file.rdbuf();
    const agora::core::BpeVocabulary vocabulary =
        agora::core::BpeVocabulary::parse_tiktoken(vocabulary_text.str());

    std::ifstream corpus(argv[3]);
    if (!corpus) {
        std::cerr << "cannot open " << argv[3] << "\n";
        return 2;
    }

    std::string line;
    std::string out;
    while (std::getline(corpus, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::uint8_t> bytes = base64_decode(line);
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (mode == "count") {
            out += std::to_string(agora::core::bpe_token_count(text, vocabulary));
            out += '\n';
        } else {
            bool first = true;
            agora::core::for_each_piece(text, [&](std::string_view piece) {
                if (!first) out += ' ';
                first = false;
                out += base64_encode(piece);
            });
            out += '\n';
        }
    }
    std::cout << out;
    return 0;
}
