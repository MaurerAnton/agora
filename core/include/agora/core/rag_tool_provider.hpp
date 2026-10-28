#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/chat_provider_context_dao.hpp"
#include "agora/core/embedding_selector.hpp"
#include "agora/core/run_recovery.hpp"
#include "agora/core/shell_tool_provider.hpp"

// tool/RagToolProvider.kt: conversation-history tools (search / list / read). The provider owns
// the tool definitions AND their execution: keyword vs semantic search (over the pinned
// BoundedSemanticEmbeddingSelector), branch reconstruction from selectedBranchesJson, window
// expansion/merging/capping and pagination. ConversationRepository, LlamaEngine and
// EmbeddingClient stay behind the seams below.

namespace agora::core::rag {

/** MIN_SEMANTIC_SEARCH_TEXT_LENGTH — the floor for both the page query and hydration. */
inline constexpr std::int32_t kMinSemanticSearchTextLength = 10;

/** ChatEntity as the search/list/read paths read it. */
struct SearchableConversation {
    std::string id;
    std::string title;
    std::int64_t last_updated = 0;
    std::optional<std::string> selected_branches_json;
};

/** MessageEntity rows the keyword search and the hydration steps read. */
struct SearchMessage {
    std::string id;
    std::string conversation_id;
    run_recovery::Participant participant = run_recovery::Participant::user;
    std::int64_t timestamp = 0;
    std::string text;
};

/**
 * The ConversationRepository seam (data/repository/ConversationRepository.kt). Every method
 * maps one Kotlin suspend call; the semantic page feeds the pinned selector's PageLoader.
 */
class ConversationSeam {
public:
    virtual ~ConversationSeam() = default;

    virtual std::vector<SearchMessage> search_messages(const std::string& query,
                                                       std::int32_t limit) = 0;

    virtual std::optional<SearchableConversation> get_searchable_conversation(
        const std::string& id) = 0;

    virtual std::vector<provider_context::MessageContextTopology> get_topology_snapshot(
        const std::string& conversation_id) = 0;

    /** getMessagesByIds — NOT searchable-filtered (used by the search hydration step). */
    virtual std::vector<SearchMessage> get_messages_by_ids(const std::vector<std::string>& ids) = 0;

    /** getSearchableMessagesByIds — semantic-path hydration. */
    virtual std::vector<SearchMessage> get_searchable_messages_by_ids(
        const std::vector<std::string>& ids) = 0;

    virtual std::int32_t get_searchable_conversation_count() = 0;

    virtual std::vector<SearchableConversation> get_searchable_conversations_page(
        std::int32_t offset, std::int32_t limit, bool descending) = 0;

    /** getEmbeddingSearchPage — rows for one keyset scan of the selector. */
    virtual std::vector<embedding::SearchRow> get_embedding_search_page(
        const std::string& model_id, std::int64_t after_id, std::int32_t minimum_text_length,
        std::int32_t limit) = 0;
};

/**
 * The query-embedding half: LlamaEngine (LOCAL) + EmbeddingClient (remote) collapse into one
 * seam. Every Kotlin failure path (model not ready, no API key, request failed) returns
 * std::nullopt — they are observably identical: an empty result list → "no_results".
 */
class EmbeddingSeam {
public:
    virtual ~EmbeddingSeam() = default;

    virtual bool is_model_ready(const std::string& local_file_path) = 0;

    virtual std::optional<std::vector<float>> compute_local_embedding(
        const std::string& query, const std::string& local_file_path) = 0;

    virtual std::optional<std::vector<float>> compute_remote_embedding(
        const std::string& query, const std::string& api_key, const std::string& model,
        const std::string& base_url) = 0;
};

class RagToolProvider final : public shell_provider::ToolProvider {
public:
    /**
     * timezone_id: SimpleDateFormat's default-zone id (java TimeZone.getDefault().id) used by
     * the "yyyy-MM-dd HH:mm:ss" timestamps. The app adapter passes the device default; tests
     * pass a fixed zone. Unknown ids resolve to GMT, exactly like TimeZone.getTimeZone.
     */
    RagToolProvider(ConversationSeam& conversations, EmbeddingSeam& embeddings,
                    std::string timezone_id = "UTC");

    std::vector<openai::ToolDefinition> definitions(
        const shell_provider::GenerationContext& ctx) const override;

    bool handles(const std::string& name) const override;

    std::string execute(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx) override;

    /** Kotlin buildSelectedTopologyBranch — private but pure, exposed for the parity tests. */
    static std::vector<provider_context::MessageContextTopology> build_selected_topology_branch(
        const std::vector<provider_context::MessageContextTopology>& all_messages,
        const std::optional<std::string>& selected_branches_json);

    /** Kotlin isSyntheticMessageId (Constants TOOL_/RESULT_/COMPACT_ prefixes). */
    static bool is_synthetic_message_id(const std::string& message_id);

    /** Participant.name for the result JSON ("USER"/"MODEL"/"ERROR"). */
    static std::string participant_name(run_recovery::Participant participant);

private:
    std::string execute_search_conversations(const std::string& arguments,
                                             const shell_provider::GenerationContext& ctx);
    std::string execute_list_conversations(const std::string& arguments,
                                           const shell_provider::GenerationContext& ctx);
    std::string execute_read_conversation(const std::string& arguments,
                                          const shell_provider::GenerationContext& ctx);

    /** semanticSearch: embed → bounded select → hydrate → text-length filter. */
    std::vector<std::pair<SearchMessage, float>> semantic_search(
        const std::string& query, std::int32_t limit,
        const shell_provider::GenerationContext& ctx);

    /** SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US) in the configured zone. */
    std::string format_timestamp(std::int64_t utc_millis) const;

    ConversationSeam& conversations_;
    EmbeddingSeam& embeddings_;
    embedding::BoundedSemanticEmbeddingSelector semantic_selector_;
    std::string timezone_id_;
};

}  // namespace agora::core::rag