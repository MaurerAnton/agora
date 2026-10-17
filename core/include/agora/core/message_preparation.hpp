#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::prep {

/**
 * The provider-neutral message-preparation pipeline, ported from the Kotlin app sources
 * (api/util/{ApiMessagePreparation,ContextWindow,ContextLimiter,ToolMessages} and the
 * cost-model estimator). Every branch is pinned by the golden traces under
 * core/tools/golden/message-preparation and must stay byte-identical.
 *
 * Failure shape: this module has no format gate; the only throwers are the split functions'
 * `require(retain >= 0)`, reported as IllegalArgumentException like the JVM.
 */
struct PrepError {
    std::string class_name;
    std::string message;
};

enum class Participant { User, Model, Error };

enum class MessageStatus { Transcribing, Sending, Thinking, ToolCalling, Success, Stopped, Error };

/**
 * The durable ChatMessage subset this pipeline reads or writes. Display-only fields (thought
 * title, token counts, run lifecycle, markdown caches) stay in Kotlin; fields the golden trace
 * projects are modeled here exactly.
 */
struct MessageSegment {
    std::string type;
    std::string content;
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_args;
    std::optional<std::string> tool_result;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> signature;
    std::optional<std::string> signature_provider;
    std::optional<std::string> tool_transcription;
    std::optional<std::string> error_code;
    std::vector<json::Value> response_output_items;
    std::optional<std::string> response_output_item_provider;
};

struct ToolCallData {
    std::string tool_name;
    std::string arguments;
    std::string result;
    std::optional<std::string> signature;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> display_name;
    std::optional<std::string> result_text;
    std::optional<std::string> structured_result;
    std::optional<std::string> transcription;
    std::vector<json::Value> response_output_items;
    std::optional<std::string> response_output_item_provider;
};

struct AttachmentItem {
    std::string type;
    std::optional<std::int32_t> image_index;
    std::optional<std::int32_t> page_count;
    std::optional<std::int64_t> file_size;
    std::optional<std::int32_t> pixel_width;
    std::optional<std::int32_t> pixel_height;
};

struct AttachmentMeta {
    std::vector<AttachmentItem> items;
};

struct ChatMessage {
    std::string id;
    std::optional<std::string> parent_id;
    std::string text;
    std::vector<std::string> images;
    std::optional<std::string> thoughts;
    std::optional<std::string> thought_title;
    std::int32_t token_count = 0;
    MessageStatus status = MessageStatus::Success;
    Participant participant = Participant::User;
    std::int64_t timestamp = 0;
    std::optional<std::string> model_name;
    std::optional<ToolCallData> tool_call;
    std::optional<std::vector<MessageSegment>> segments;
    std::optional<AttachmentMeta> attachment_meta;
    std::optional<std::string> run_id;
    std::optional<std::int64_t> run_sequence;

    /** ChatMessage.isToolProtocolMessage: a tool_ or result_ row. */
    bool is_tool_protocol_message() const;
    bool is_context_compact() const;
    bool is_successful_context_compact() const;
};

// ---------------------------------------------------------------------------------------------
// Cost model — api/util/tokens/*
// ---------------------------------------------------------------------------------------------

struct EnvelopeCost {
    std::int32_t per_message = 0;
    std::int32_t per_tool_definition = 0;
    std::int32_t per_tool_call = 0;
    std::int64_t request_overhead = 0;
    std::int64_t tool_set_overhead = 0;

    /** The shared historical framing: 8 tokens per message, 16 per tool schema or call. */
    static EnvelopeCost default_cost();
};

struct SafetyMargin {
    std::int64_t numerator = 11;
    std::int64_t denominator = 10;

    /** Rounds up, so the margin never disappears on small counts. */
    int apply(std::int64_t raw) const;
};

struct ImageDescriptor {
    std::optional<std::int64_t> byte_size;
    std::optional<std::int32_t> pixel_width;
    std::optional<std::int32_t> pixel_height;
};

/** TextTokenCounter: pure, offline counting of one string. */
using TextTokenCounter = std::function<std::int64_t(std::string_view)>;

/** ImageTokenCost: raw (margin-free) price of one image descriptor. */
using ImageTokenCostFn = std::function<std::int64_t(const ImageDescriptor&)>;

struct ContextCostModel {
    TextTokenCounter text;
    ImageTokenCostFn image;
    EnvelopeCost envelope;
    SafetyMargin safety_margin;

    /** Model-independent default: character heuristic, byte-proportional images, shared framing. */
    static ContextCostModel default_model();
};

/** HeuristicTextTokenCounter: ASCII word-like runs at 4 chars per token, other code points 1 each. */
std::int64_t heuristic_count(std::string_view text);

/** ByteProportionalImageCost. */
std::int64_t byte_proportional_image_tokens(const ImageDescriptor& image);

/** OpenAiTileImageCost / OpenAiPatchImageCost (the two published OpenAI image rules). */
std::int64_t openai_tile_image_tokens(std::int32_t width, std::int32_t height, std::int32_t base_tokens,
                                      std::int32_t tile_tokens);
std::int64_t openai_patch_image_tokens(std::int32_t width, std::int32_t height, std::int32_t max_edge_px,
                                       std::optional<std::int32_t> patch_budget, double multiplier);

/** AnthropicImageCost for the standard / high-resolution tier. */
std::int64_t anthropic_image_tokens(const ImageDescriptor& image, bool high_resolution);

/** GeminiImageCost. */
std::int64_t gemini_image_tokens(const ImageDescriptor& image);

/** ModelThinkingCapability-style family selection for token costs (ContextCostModels). */
enum class ContextCostFamily { Anthropic, OpenAi, Gemini, Local, Generic };

/** ModelId.parse: "Provider:model" or the legacy unprefixed heuristics. */
struct PrefixedModelId {
    std::string provider_name;
    std::string model_name;
};

PrefixedModelId parse_prefixed_model_id(const std::string& prefixed);
ContextCostFamily family_of(const std::optional<PrefixedModelId>& model_id);

/**
 * ContextCostModels.forModel. [o200k_counter] prices text for every remote family, exactly like
 * the app's Kotlin BPE counter over the shipped vocabulary; LOCAL falls back to the heuristic
 * (the embedded runtime is out of this slice, and the app does the same before a model loads).
 */
ContextCostModel cost_model_for(const std::optional<PrefixedModelId>& model_id,
                                const TextTokenCounter& o200k_counter);

struct FixedContextComposition {
    std::int32_t system_prompt_tokens = 0;
    std::int32_t tool_tokens = 0;
};

/** CostModelContextEstimator: totals what a provider will see, asking the cost model for prices. */
class CostModelContextEstimator {
public:
    explicit CostModelContextEstimator(ContextCostModel costs);

    int estimate(const std::vector<ChatMessage>& messages, bool include_assistant_reasoning) const;

    int estimate_fixed(const std::optional<std::string>& system_prompt,
                       const std::vector<openai::ToolDefinition>& tools,
                       const std::optional<std::string>& initial_user_prompt,
                       bool code_execution_enabled, bool google_search_enabled,
                       bool open_ai_web_search_enabled) const;

    FixedContextComposition estimate_fixed_composition(
        const std::optional<std::string>& system_prompt,
        const std::vector<openai::ToolDefinition>& tools,
        const std::optional<std::string>& initial_user_prompt, bool code_execution_enabled,
        bool google_search_enabled, bool open_ai_web_search_enabled) const;

    int estimate_text(std::string_view text) const;

    std::int64_t image_tokens(const ImageDescriptor& image) const;

private:
    ContextCostModel costs_;
};

// ---------------------------------------------------------------------------------------------
// Transforms — ToolMessages.kt / ApiMessagePreparation.kt / ContextWindow.kt / ContextLimiter.kt
// ---------------------------------------------------------------------------------------------

/** stripEmptyTurns: drops turns that would serialize to an empty/whitespace-only content block. */
std::vector<ChatMessage> strip_empty_turns(const std::vector<ChatMessage>& messages);

/** mergeConsecutiveSameRole: joins consecutive non-tool rows that share the participant. */
std::vector<ChatMessage> merge_consecutive_same_role(const std::vector<ChatMessage>& messages);

/** validateToolMessages: normalizes complete tool_ / result_ rounds, or replays them as context. */
std::vector<ChatMessage> validate_tool_messages(const std::vector<ChatMessage>& messages);

/** adaptToolRoundsForProvider: downgrades tool rounds whose opaque protocol metadata (thought
 *  signatures) cannot be replayed on the target provider to inert archived user context. */
std::vector<ChatMessage> adapt_tool_rounds_for_provider(
    const std::vector<ChatMessage>& messages, const std::string& provider_name,
    const std::function<bool(const ChatMessage&)>& is_compatible);

/** applyNearestContextCompact: non-destructive Compact projection. */
std::vector<ChatMessage> apply_nearest_context_compact(const std::vector<ChatMessage>& messages);

/** canonicalContextMessages: compact projection + dedup + tool validation + strip + merge. */
std::vector<ChatMessage> canonical_context_messages(const std::vector<ChatMessage>& messages);

/** prepareMessages: the full fail-closed request-boundary pipeline. */
std::vector<ChatMessage> prepare_messages(const std::vector<ChatMessage>& messages,
                                          int context_token_budget,
                                          bool include_assistant_reasoning,
                                          const ContextCostModel& costs);

/** limitContext: keeps the newest protocol-complete suffix that fits the token budget. */
std::vector<ChatMessage> limit_context(const std::vector<ChatMessage>& messages,
                                       int context_token_budget, bool include_assistant_reasoning,
                                       const ContextCostModel& costs);

/** protocolAtomicUnits: a tool_ row with its result_ rows is one indivisible unit. */
std::vector<std::vector<ChatMessage>> protocol_atomic_units(const std::vector<ChatMessage>& messages);

struct ContextRetentionSplit {
    std::vector<ChatMessage> prefix;
    std::vector<ChatMessage> retained;
    std::int32_t retained_message_count = 0;
};

ContextRetentionSplit split_context_for_compact_retention(const std::vector<ChatMessage>& messages,
                                                          int retain_messages);

struct LogicalContextSplit {
    std::vector<ChatMessage> prefix;
    std::vector<ChatMessage> suffix;
    std::int32_t logical_message_count = 0;
};

LogicalContextSplit split_logical_context(const std::vector<ChatMessage>& messages,
                                          int retain_logical_messages);

struct ContextWindowUsage {
    std::int32_t estimated_token_count = 0;
    std::int32_t token_budget = 0;
    std::int32_t logical_message_count = 0;
    bool has_compact_boundary = false;
    std::int32_t system_prompt_tokens = 0;
    std::int32_t tool_tokens = 0;

    float progress() const;
    std::int32_t message_tokens() const;
};

ContextWindowUsage context_window_usage(const std::vector<ChatMessage>& messages,
                                        int token_budget, int fixed_token_cost,
                                        bool include_assistant_reasoning,
                                        const std::optional<FixedContextComposition>& fixed_composition,
                                        const ContextCostModel& costs);

/** contextWindowRetainedMessageIds: the original ids the provider window keeps. */
std::vector<std::string> context_window_retained_message_ids(const std::vector<ChatMessage>& messages,
                                                             int token_budget, int fixed_token_cost,
                                                             bool include_assistant_reasoning,
                                                             const ContextCostModel& costs);

/** projectAssistantImagesToLatestUserMessage. */
std::vector<ChatMessage> project_assistant_images_to_latest_user_message(
    const std::vector<ChatMessage>& messages, bool include_images);

/** projectToolResultImagesToUserMessage. */
std::vector<ChatMessage> project_tool_result_images_to_user_message(
    const std::vector<ChatMessage>& messages, bool include_images);

/** projectGenerationStatusesForApi; [generation_error_formatter] is the caller's presentation. */
std::vector<ChatMessage> project_generation_statuses_for_api(
    const std::vector<ChatMessage>& messages,
    const std::function<std::string(const std::string&)>& generation_error_formatter);

}  // namespace agora::core::prep
