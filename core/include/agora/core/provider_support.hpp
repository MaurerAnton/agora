#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "agora/core/stream_normalization.hpp"

namespace agora::core::provider_support {

namespace sn = agora::core::stream_normalization;

// ---------------------------------------------------------------------------------------------
// HTTP error body mapping — ProviderHttpError.kt. Used by every provider's non-200 branch and
// by the error presentation layer.
// ---------------------------------------------------------------------------------------------

struct ParsedProviderHttpErrorBody {
    std::optional<std::string> code;
    std::optional<std::string> type;
    std::string message;
    bool structured = false;
};

/** Parses an error body: JSON object (nested `error` preferred), primitive or free text.
 *  Returns nullopt only for a blank body. */
std::optional<ParsedProviderHttpErrorBody> parse_provider_http_error_body(std::string_view raw_body);

/** providerHttpError: the Api error for a non-200 response (rawBody may be absent). */
sn::ErrorApi provider_http_error(std::int32_t status_code, const std::optional<std::string>& raw_body);

/** extractStructuredProviderHttpErrorMessage: the message of a structured body, else null. */
std::optional<std::string> extract_structured_provider_http_error_message(std::string_view raw_body);

// ---------------------------------------------------------------------------------------------
// BaseUrlResolver.kt — protocol-version handling for custom endpoint base URLs.
// ---------------------------------------------------------------------------------------------

/** Matches `/v\d` anywhere in the trailing-slash-trimmed URL. */
bool has_version_segment(std::string_view url);

/** Appends `/v1` unless the URL is blank or already carries a version segment. */
std::string with_v1(std::string_view url);

/** Removes only a terminal API version segment; null when there is none (or nothing remains). */
std::optional<std::string> without_trailing_version(std::string_view url);

// ---------------------------------------------------------------------------------------------
// ProviderDefaults.kt — default base URLs (chat fallback + embedding provider table).
// ---------------------------------------------------------------------------------------------

inline constexpr std::string_view kOpenAiBaseUrl = "https://api.openai.com/v1";

/** isOpenAiCompatibleEmbedding: exact display-name match on "OpenAI" / "Open Router". */
bool is_openai_compatible_embedding(std::string_view name);

/** openAiCompatibleBaseUrl: the configured OpenAI URL, else the default. */
std::string open_ai_compatible_base_url(const std::map<std::string, std::string>& base_urls);

/** embeddingBaseUrl: provider display name (ASCII case-insensitive) → default endpoint. */
std::string embedding_base_url(std::string_view provider);

}  // namespace agora::core::provider_support
