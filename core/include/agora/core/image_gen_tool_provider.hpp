#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/shell_tool_provider.hpp"

// tool/ImageGenToolProvider.kt: the `generate_image` tool over an OpenAI-compatible
// `/images/generations` endpoint. The OkHttp calls and the Android ToolImageStore's
// BitmapFactory/persistGeneratedBytes half are seams; everything else (the enabled gate,
// argument decoding, request body, response walk, base64 decode, envelopes) is pure.

namespace agora::core::imagegen {

// util/Constants.kt IMAGE_GENERATION_TIMEOUT_MS + api/ProviderDefaults.kt OPENAI_BASE_URL.
inline constexpr std::int64_t kImageGenerationTimeoutMs = 600'000;
inline constexpr const char* kOpenAiBaseUrl = "https://api.openai.com/v1";

/** ToolImageStore.persistGeneratedBytes — Android mime detection + file publication. */
class ImageStore {
public:
    virtual ~ImageStore() = default;
    virtual shell_provider::ToolImageAttachment persist_generated_bytes(
        const std::string& bytes, const std::string& file_prefix) = 0;
};

/**
 * api/HttpClient.kt's post/getBytes (the OkHttp half):
 *   - post_json returns std::nullopt exactly where HttpClient.post returns null (a non-2xx
 *     status); IOExceptions propagate as exceptions, which the provider folds into
 *     "generation_error" like its Kotlin catch.
 *   - get_bytes returns std::nullopt exactly where getBytes returns null.
 * The production adapter also enforces HttpClient.guardCleartextCredentials.
 */
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual std::optional<std::string> post_json(
        const std::string& url, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::int64_t call_timeout_ms, std::int64_t read_timeout_ms) = 0;
    virtual std::optional<std::string> get_bytes(const std::string& url,
                                                 std::int64_t call_timeout_ms,
                                                 std::int64_t read_timeout_ms) = 0;
};

class ImageGenToolProvider final : public shell_provider::ToolProvider {
public:
    ImageGenToolProvider(HttpTransport& http, ImageStore& image_store);

    std::vector<openai::ToolDefinition> definitions(
        const shell_provider::GenerationContext& ctx) const override;

    /** Kotlin execute: `executeResult(...).text`. */
    std::string execute(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx) override;

    /** Kotlin executeResult: the images list and the isError flag live here. */
    shell_provider::ToolExecutionResult execute_one_shot_result(
        const std::string& name, const std::string& arguments,
        const shell_provider::GenerationContext& ctx) override;

    bool handles(const std::string& name) const override;

private:
    /** Kotlin executeOffMain: (text, attachment-or-null). */
    std::pair<std::string, std::optional<shell_provider::ToolImageAttachment>> execute_off_main(
        const std::string& name, const std::string& arguments,
        const shell_provider::GenerationContext& ctx);

    HttpTransport& http_;
    ImageStore& image_store_;
};

}  // namespace agora::core::imagegen
