#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "agora/core/bpe_vocabulary.hpp"

namespace agora::core {

/** Prices one string in provider-visible tokens, before any safety margin. */
class TextCounter {
public:
    virtual ~TextCounter() = default;
    virtual std::int64_t count(std::string_view utf8) const = 0;
};

/**
 * Character-class heuristic used when no real vocabulary is available for a model.
 *
 * Port of HeuristicTextTokenCounter: ASCII word-like runs cost roughly one token per four
 * characters; every other code point costs one token on its own, which keeps CJK and
 * punctuation-heavy text from being under-counted. Whitespace only terminates a run.
 */
class HeuristicTextCounter final : public TextCounter {
public:
    static const HeuristicTextCounter& instance();
    std::int64_t count(std::string_view utf8) const override;
};

/**
 * Text counting through a real BPE vocabulary, with the heuristic as a fallback.
 *
 * Port of BpeTextTokenCounter, including the memo: the context indicator re-prices the whole
 * selected history on every projection, and almost all of that text is unchanged since the
 * last time, so raw counts of long texts are remembered under a character budget.
 */
class BpeTextCounter final : public TextCounter {
public:
    /**
     * `vocabulary` may be null, in which case every count falls back. `fallback` and
     * `vocabulary` must outlive this counter. Throws std::invalid_argument for scale < 1.0,
     * which would claim a family is cheaper than the vocabulary says.
     */
    BpeTextCounter(const BpeVocabulary* vocabulary, double scale, const TextCounter* fallback);

    std::int64_t count(std::string_view utf8) const override;

private:
    using Entry = std::pair<std::string, std::int64_t>;
    using EntryIterator = std::list<Entry>::iterator;

    struct TransparentHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const;
        std::size_t operator()(const std::string& text) const {
            return operator()(std::string_view(text));
        }
    };

    std::int64_t memoized(std::string_view utf8) const;
    std::int64_t count_uncached(std::string_view utf8) const;

    const BpeVocabulary* vocabulary_;
    double scale_;
    const TextCounter* fallback_;

    mutable std::mutex mutex_;
    mutable std::list<Entry> entries_;  // most recently used first
    mutable std::unordered_map<std::string, EntryIterator, TransparentHash, std::equal_to<>>
        index_;
    mutable std::int64_t stored_chars_ = 0;
};

}  // namespace agora::core
