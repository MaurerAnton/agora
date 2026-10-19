#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace agora::core::embedding {

/** One keyset projection of an embedding row, as data/local/EmbeddingSearchRow.kt declares it. */
struct SearchRow {
    std::int64_t id = 0;
    std::string message_id;
    std::vector<std::uint8_t> embedding;  // big-endian IEEE-754 floats
    int dimension = 0;
};

struct Candidate {
    std::int64_t row_id = 0;
    std::string message_id;
    float score = 0.0f;
};

struct Selection {
    std::vector<Candidate> candidates;  // score descending, then row id ascending
    int scanned_rows = 0;
    int skipped_invalid_rows = 0;
    float best_score = 0.0f;
    int max_retained_candidates = 0;
};

/**
 * Native port of tool/BoundedSemanticEmbeddingSelector.kt.
 *
 * Scores one keyset page at a time and retains only the best `limit` candidates, so a large
 * embedding cache never has to fit in memory at once. Rows arriving out of order, pages larger
 * than the requested bound, malformed vectors and non-finite scores are all handled exactly as
 * the Kotlin does — the parity test pins each of those messages and counts.
 */
class BoundedSemanticEmbeddingSelector {
public:
    using PageLoader = std::function<std::vector<SearchRow>(std::int64_t after_id, int limit)>;

    /** @throws std::invalid_argument("Failed requirement.") when page_size <= 0, like Kotlin. */
    explicit BoundedSemanticEmbeddingSelector(int page_size = 256);

    Selection select(const std::vector<float>& query_embedding, float threshold, int limit,
                     const PageLoader& load_page) const;

private:
    int page_size_;
};

}  // namespace agora::core::embedding
