#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/duckduckgo.hpp"
#include "agora/core/shell_tool_provider.hpp"

// tool/WebSearchToolProvider.kt: the web_search/web_fetch tools. The transport half
// (api/HttpClient.kt post/fetchModels + DuckDuckGoScraper.search) and Android's
// HtmlCompat.fromHtml stay behind the seams below — the provider owns the definitions,
// the argument coercion, every provider's request shape and the normalized result JSON,
// which is what the parity tests pin.

namespace agora::core::websearch {

// util/Constants.kt (WEB_FETCH_USER_AGENT, MAX_WEB_FETCH_HTML_LENGTH, NETWORK_TOOL_TIMEOUT_MS).
inline constexpr const char* kWebFetchUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";
inline constexpr std::int64_t kMaxWebFetchHtmlLength = 600'000;
inline constexpr std::int64_t kNetworkToolTimeoutMs = 60'000;

/**
 * Android HtmlCompat.fromHtml(FROM_HTML_MODE_COMPACT): decodes entities and flattens the
 * remaining markup into text (block elements become newlines). The provider runs the pure
 * strip passes first, exactly like htmlToReadableText does before HtmlCompat.
 */
using HtmlToText = std::function<std::string(const std::string& html)>;

/**
 * api/HttpClient.kt's post/fetchModels plus DuckDuckGoScraper(webClient).search — the
 * network half WebSearchToolProvider.kt leaves to Kotlin:
 *   - post_json / get_text return std::nullopt exactly where Kotlin returns null
 *     (non-2xx status, or an empty POST body); IOExceptions propagate as exceptions,
 *     which the provider folds into "search_error"/"fetch_error" like its Kotlin catch.
 *   - search_duckduckgo wraps the scraper call (the pinned duckduckgo::search loop is the
 *     engine behind the production implementation; tests script the outcome directly).
 */
class Transport {
public:
    virtual ~Transport() = default;

    virtual std::optional<std::string> post_json(
        const std::string& url, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::int64_t call_timeout_ms) = 0;

    virtual std::optional<std::string> get_text(
        const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
        std::int64_t call_timeout_ms) = 0;

    virtual duckduckgo::SearchOutcome search_duckduckgo(const std::string& query,
                                                        std::int32_t num_results) = 0;
};

// ---- Pure helpers (each mirrors one Kotlin top-level/internal function) ---------------------

/** SettingsContracts.kt normalizeWebSearchProvider: trim + lowercase + membership, else duckduckgo. */
std::string normalize_web_search_provider(const std::optional<std::string>& provider);

/** WebSearchToolProvider.webSearchProviderDisplayName (Kotlin `when`, else → "Brave Search"). */
std::string web_search_provider_display_name(const std::string& provider);

/** URLEncoder.encode(text, "UTF-8"): space → '+', unreserved ASCII kept, rest %XX uppercase. */
std::string url_encode_form(std::string_view text);

/** searxngSearchUrl: blank → https://searx.be, trimEnd('/'), `?q=<encoded>&format=json`. */
std::string searxng_search_url(const std::string& configured_base_url, const std::string& query);

/** kagiSearchRequestBody: `{"query":…,"workflow":"search","limit":clamp(1,10)}` compact. */
std::string kagi_search_request_body(const std::string& query, std::int32_t num_results);

/** normalizeKagiSearchResponse: data.search → capped {title,url,description} results. */
std::string normalize_kagi_search_response(const std::string& response_body,
                                           const std::string& query, std::int32_t num_results);

/**
 * htmlToReadableText: the 600 KB cap, comment/script/style/head/nav/header/footer/aside
 * strip passes, then [decode] (HtmlCompat), then the intra-line/blank-run collapse and trim.
 */
std::string html_to_readable_text(const std::string& raw_html, const HtmlToText& decode);

// ---- The provider ------------------------------------------------------------------------------

class WebSearchToolProvider final : public shell_provider::ToolProvider {
public:
    WebSearchToolProvider(Transport& transport, HtmlToText html_to_text);

    std::vector<openai::ToolDefinition> definitions(
        const shell_provider::GenerationContext& ctx) const override;

    std::string execute(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx) override;

    bool handles(const std::string& name) const override;

private:
    std::string execute_web_search(const std::string& arguments,
                                   const shell_provider::GenerationContext& ctx);
    std::string execute_web_fetch(const std::string& arguments,
                                  const shell_provider::GenerationContext& ctx);

    Transport& transport_;
    HtmlToText html_to_text_;
};

}  // namespace agora::core::websearch