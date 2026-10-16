#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "agora/core/bpe_vocabulary.hpp"

namespace agora::core {

/**
 * Counts tokens the way a byte-level BPE tokenizer would, without building the token list.
 *
 * Port of BpeTokenCount.kt: the text is split by the o200k pre-tokenizer pattern, then each
 * piece is merged by rank until no pair can merge again; how many parts are left is the count.
 */
std::int64_t bpe_token_count(std::string_view utf8, const BpeVocabulary& vocabulary);

/**
 * Calls `visit` with every piece the o200k pre-tokenizer produces, in order. Exposed so tooling
 * can compare segmentation with the Kotlin implementation piece by piece; counting uses the same
 * scan without materialising the list.
 */
void for_each_piece(std::string_view utf8, const std::function<void(std::string_view)>& visit);

}  // namespace agora::core
