#include "agora/core/embedding_selector.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <queue>
#include <stdexcept>

namespace agora::core::embedding {
namespace {

constexpr int kFloatSizeBytes = 4;

/** Kotlin Float.fromBits over a big-endian four-byte read. */
float decode_big_endian(const std::uint8_t* bytes) {
    const std::uint32_t bits = (static_cast<std::uint32_t>(bytes[0]) << 24) |
                               (static_cast<std::uint32_t>(bytes[1]) << 16) |
                               (static_cast<std::uint32_t>(bytes[2]) << 8) |
                               static_cast<std::uint32_t>(bytes[3]);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/** Port of the selector's private DirectEmbeddingCosineScorer. */
class DirectEmbeddingCosineScorer {
public:
    explicit DirectEmbeddingCosineScorer(const std::vector<float>& query) : query_(query) {
        float norm = 0.0f;
        for (float value : query_) norm += value * value;
        query_magnitude_ = std::sqrt(norm);
    }

    float score(const std::vector<std::uint8_t>& bytes) const {
        if (bytes.size() != query_.size() * static_cast<std::size_t>(kFloatSizeBytes)) {
            throw std::invalid_argument("Failed requirement.");
        }
        float dot = 0.0f;
        float norm_b = 0.0f;
        for (std::size_t index = 0; index < query_.size(); ++index) {
            const float value_b = decode_big_endian(bytes.data() + index * kFloatSizeBytes);
            dot += query_[index] * value_b;
            norm_b += value_b * value_b;
        }
        const float denominator = query_magnitude_ * std::sqrt(norm_b);
        return denominator == 0.0f ? 0.0f : dot / denominator;
    }

private:
    const std::vector<float>& query_;
    float query_magnitude_ = 0.0f;
};

/**
 * The retention heap keeps the best candidates while discarding the worst one on overflow, so
 * `top()` has to be the worst retained candidate: lowest score, and — the tie-break that decides
 * which of two equally scoring rows survives — the highest row id.
 */
struct WorstIsTop {
    bool operator()(const Candidate& a, const Candidate& b) const {
        if (a.score != b.score) return a.score > b.score;
        return a.row_id < b.row_id;
    }
};

/** Kotlin's replacement test: strictly better score, or equal score with a lower row id. */
bool outranks(const Candidate& candidate, const Candidate& worst) {
    return candidate.score > worst.score ||
           (candidate.score == worst.score && candidate.row_id < worst.row_id);
}

}  // namespace

BoundedSemanticEmbeddingSelector::BoundedSemanticEmbeddingSelector(int page_size)
    : page_size_(page_size) {
    if (page_size_ <= 0) throw std::invalid_argument("Failed requirement.");
}

Selection BoundedSemanticEmbeddingSelector::select(const std::vector<float>& query_embedding,
                                                   float threshold, int limit,
                                                   const PageLoader& load_page) const {
    if (query_embedding.empty() || limit <= 0) {
        return Selection{};
    }

    const DirectEmbeddingCosineScorer scorer(query_embedding);
    std::priority_queue<Candidate, std::vector<Candidate>, WorstIsTop> retained;
    std::int64_t after_id = 0;
    int scanned_rows = 0;
    int skipped_invalid_rows = 0;
    bool has_best = false;
    float best_score = 0.0f;
    int max_retained_candidates = 0;

    while (true) {
        const std::vector<SearchRow> page = load_page(after_id, page_size_);
        if (page.size() > static_cast<std::size_t>(page_size_)) {
            throw std::invalid_argument("Embedding page exceeded the requested bound");
        }
        if (page.empty()) break;

        std::int64_t previous_id = after_id;
        for (const SearchRow& row : page) {
            if (row.id <= previous_id) {
                throw std::invalid_argument(
                    "Embedding keyset page must be strictly ordered after id " +
                    std::to_string(after_id));
            }
            previous_id = row.id;
            ++scanned_rows;

            const auto expected_bytes =
                static_cast<std::int64_t>(query_embedding.size()) * kFloatSizeBytes;
            if (row.dimension != static_cast<int>(query_embedding.size()) || row.dimension <= 0 ||
                static_cast<std::int64_t>(row.embedding.size()) != expected_bytes) {
                ++skipped_invalid_rows;
                continue;
            }

            // Kotlin wraps the scoring in runCatching: a failure counts as a skipped row rather
            // than aborting the search.
            std::optional<float> score;
            try {
                score = scorer.score(row.embedding);
            } catch (const std::exception&) {
                score.reset();
            }
            if (!score || !std::isfinite(*score)) {
                ++skipped_invalid_rows;
                continue;
            }
            best_score = has_best ? std::max(best_score, *score) : *score;
            has_best = true;
            if (*score <= threshold) continue;

            const Candidate candidate{row.id, row.message_id, *score};
            if (retained.size() < static_cast<std::size_t>(limit)) {
                retained.push(candidate);
            } else if (outranks(candidate, retained.top())) {
                retained.pop();
                retained.push(candidate);
            }
            if (retained.size() > static_cast<std::size_t>(max_retained_candidates)) {
                max_retained_candidates = static_cast<int>(retained.size());
            }
        }

        after_id = page.back().id;
        if (page.size() < static_cast<std::size_t>(page_size_)) break;
    }

    Selection selection;
    selection.scanned_rows = scanned_rows;
    selection.skipped_invalid_rows = skipped_invalid_rows;
    selection.best_score = has_best ? best_score : 0.0f;
    selection.max_retained_candidates = max_retained_candidates;

    selection.candidates.reserve(retained.size());
    while (!retained.empty()) {
        selection.candidates.push_back(retained.top());
        retained.pop();
    }
    std::sort(selection.candidates.begin(), selection.candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.row_id < b.row_id;
              });
    return selection;
}

}  // namespace agora::core::embedding
