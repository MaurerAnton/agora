#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::embedding {

// ---------------------------------------------------------------------------------------------
// Request encoding — EmbeddingClient.kt's @Serializable EmbeddingRequest / BatchEmbeddingRequest
// (extracted verbatim by the stand; kotlinx writes input before model).
// ---------------------------------------------------------------------------------------------

std::string write_embedding_request(std::string_view text, std::string_view model);
std::string write_batch_embedding_request(const std::vector<std::string>& texts,
                                           std::string_view model);

// ---------------------------------------------------------------------------------------------
// Response parsing — the two compute* functions' response halves (the transport boundary is the
// HTTP call, which stays in Kotlin; every structural/numeric failure lands in their catch).
// ---------------------------------------------------------------------------------------------

/** computeEmbedding: the joined values in kotlinx Float.toString spelling, or nullopt for a
 *  null response and every structural/numeric failure. */
std::optional<std::string> parse_embedding_values(
    const std::optional<std::string_view>& response);

/** computeEmbeddings: one entry per data item on success (the response length wins), one null
 *  per text on the early paths and on any exception — an explicit `"embedding": null` is a
 *  JsonNull.jsonArray failure, i.e. all-nulls, not a per-item miss. */
std::vector<std::optional<std::string>> parse_batch_embedding_entries(
    const std::optional<std::string_view>& response, std::size_t count);

}  // namespace agora::core::embedding
