#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/chat_entities.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::chat_search {

/** EmbeddingSearchRow (data/local/EmbeddingSearchRow.kt): the scoring projection. */
struct EmbeddingSearchRow {
    std::int64_t id = 0;
    std::string message_id;
    std::vector<std::uint8_t> embedding;
    int dimension = 0;
};

/** EmbeddingModelCount (data/local/ChatSearchDao.kt): one GROUP BY row. */
struct EmbeddingModelCount {
    std::string model_id;
    int count = 0;
};

/** IndexableMessage (data/local/ChatEntities.kt): the id/text pair an embedding needs. */
struct IndexableMessage {
    std::string id;
    std::string text;
};

/**
 * Port of data/local/ChatSearchDao.kt: the search-eligibility and bounded-projection surface
 * inherited by the sole ChatDao.
 *
 * All fourteen members are @Query reads; the SQL text is transcribed verbatim from the
 * annotations (the LIKE patterns are used exactly as declared, including the caller-escaped
 * `query` parameter and the `_` wildcard inside the NOT LIKE id filters) and Room is replaced by
 * the SqlExecutor boundary. Collection parameters expand the way Room's generated adapters
 * expand them: one placeholder per element and binding NULL for an empty list, which SQLite
 * treats as a never-matching `IN (NULL)`.
 */
class ChatSearchDao {
public:
    explicit ChatSearchDao(SqlExecutor& sql) : sql_(sql) {}

    /** [query] must be pre-escaped for LIKE (ConversationRepository.escapeLikePattern). */
    std::vector<MessageEntity> search_messages(std::string_view query, int limit = 10);

    std::vector<MessageEntity> get_messages_with_citation_segments_page(
        std::string_view after_id,
        int page_size);

    std::vector<EmbeddingSearchRow> get_embedding_search_page(std::string_view model_id,
                                                              std::int64_t after_id,
                                                              int minimum_text_length,
                                                              int limit);

    int get_embedding_count_by_model(std::string_view model_id);

    std::vector<EmbeddingModelCount> get_embedding_counts_by_models(
        const std::vector<std::string>& model_ids);

    int get_indexable_message_count();

    std::vector<IndexableMessage> get_searchable_messages_page(
        const std::optional<std::string>& after_id,
        int limit);

    std::vector<IndexableMessage> get_unembedded_messages_page(
        std::string_view model_id,
        const std::optional<std::string>& after_id,
        int limit);

    std::vector<MessageEntity> get_searchable_messages_by_ids(
        const std::vector<std::string>& ids);

    bool is_message_searchable(std::string_view message_id);

    std::optional<ChatEntity> get_searchable_conversation(std::string_view conversation_id);

    int get_searchable_conversation_count();

    std::vector<ChatEntity> get_searchable_conversations_page_ascending(int offset, int limit);

    std::vector<ChatEntity> get_searchable_conversations_page_descending(int offset, int limit);

private:
    SqlExecutor& sql_;
};

}  // namespace agora::core::chat_search
