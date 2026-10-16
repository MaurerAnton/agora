#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace agora::core {

/**
 * A byte-level BPE vocabulary read from the `.tiktoken` format.
 *
 * Port of BpeVocabulary.kt: token bytes are concatenated once and looked up through an
 * open-addressed index, so a merge loop can probe without allocating.
 */
class BpeVocabulary {
public:
    /** Returned by rank_of when the byte range is not one token of this vocabulary. */
    static constexpr int kAbsent = -1;

    /**
     * Parses the `.tiktoken` format: one line per token, holding its base64 bytes and its rank.
     * Malformed lines are skipped, matching the Kotlin loader.
     */
    static BpeVocabulary parse_tiktoken(std::string_view text);

    std::size_t size() const { return ranks_.size(); }

    /** The rank of `bytes[from, to)`, or kAbsent. Lower ranks merge earlier. */
    int rank_of(const std::uint8_t* bytes, std::size_t from, std::size_t to) const;
    int rank_of(std::string_view bytes) const {
        return rank_of(reinterpret_cast<const std::uint8_t*>(bytes.data()), 0, bytes.size());
    }

private:
    std::vector<std::uint8_t> token_bytes_;
    std::vector<std::uint32_t> offsets_;  // size() + 1 entries
    std::vector<std::int32_t> ranks_;
    std::vector<std::uint32_t> buckets_;
};

}  // namespace agora::core
