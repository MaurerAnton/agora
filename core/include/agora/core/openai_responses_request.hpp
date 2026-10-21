#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::openai_responses {

// The chat request models come from the already-ported openai_request module.
using openai::OpenAiMessage;
using openai::OpenAiReasoning;
using openai::ToolDefinition;
using openai::ToolParameters;

/**
 * Port of api/openai/OpenAiRequestValidator.kt: the OpenAI Responses request builder and the
 * wire-format gate.
 *
 *   toResponsesInput — the chat messages become Responses input items; a stored reasoning item is
 *       replayed only when the provider *and* the model match, otherwise the plain function calls
 *       are rebuilt from the stored tool calls.
 *   toResponsesTools — the chat tool definitions become Responses tools.
 *   requireValidWireFormat — the local gate that proves the request satisfies the Responses
 *       grammar before the request is opened (every violation is collected and reported).
 *
 * The request models (OpenAiResponsesRequest, OpenAiResponseInputItem, OpenAiResponseInputContent,
 * OpenAiResponseTool) come from api/LlmProvider.kt; the golden stand lifts them byte-for-byte.
 */

/** api/LlmProvider.kt::OpenAiResponseInputContent. */
struct ResponseInputContent {
    std::string type;
    std::optional<std::string> text;
    std::optional<std::string> image_url;
    std::optional<std::string> detail;
};

/** api/LlmProvider.kt::OpenAiResponseInputItem. */
struct ResponseInputItem {
    std::string type;
    std::optional<std::string> id;
    std::optional<std::string> role;
    std::optional<std::vector<ResponseInputContent>> content;
    std::optional<json::Value> summary;
    std::optional<std::string> encrypted_content;
    std::optional<std::string> call_id;
    std::optional<std::string> name;
    std::optional<std::string> arguments;
    std::optional<json::Value> output;
};

/** api/LlmProvider.kt::OpenAiResponseTool. */
struct ResponseTool {
    std::string type = "function";
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<ToolParameters> parameters;
};

/** api/LlmProvider.kt::OpenAiResponsesRequest. */
struct ResponsesRequest {
    std::string model;
    std::vector<json::Value> input;
    bool stream = true;
    std::optional<std::vector<ResponseTool>> tools;
    std::optional<OpenAiReasoning> reasoning;
    std::optional<std::string> service_tier;
    std::optional<float> temperature;
    std::optional<int> max_output_tokens;
    std::optional<float> top_p;
    std::optional<std::string> prompt_cache_key;
};

/** api/util/RequestFormatValidation.kt::RequestFormatException. */
class RequestFormatException : public std::runtime_error {
public:
    RequestFormatException(std::string provider, std::vector<std::string> violations);

    const std::string& provider() const { return provider_; }
    const std::vector<std::string>& violations() const { return violations_; }

private:
    std::string provider_;
    std::vector<std::string> violations_;
};

/** toResponsesInput(messages, providerName, targetModel). */
std::vector<json::Value> to_responses_input(
    const std::vector<OpenAiMessage>& messages,
    const std::optional<std::string>& provider_name = std::nullopt,
    const std::optional<std::string>& target_model = std::nullopt);

/** toResponsesTools(tools). */
std::vector<ResponseTool> to_responses_tools(const std::vector<ToolDefinition>& tools);

/** requireValidWireFormat(provider): throws RequestFormatException with every violation. */
void require_valid_wire_format(const ResponsesRequest& request, const std::string& provider);

}  // namespace agora::core::openai_responses
