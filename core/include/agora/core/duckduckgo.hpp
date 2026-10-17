#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::duckduckgo {

// ---------------------------------------------------------------------------------------------
// Result model — DuckDuckGoScraper.WebResult (extracted verbatim by the stand).
// ---------------------------------------------------------------------------------------------

struct WebResult {
    std::string title;
    std::string url;
    std::string snippet;
};

// ---------------------------------------------------------------------------------------------
// The parsing pieces of DuckDuckGoScraper.kt (extracted verbatim by the stand; these replicate
// the pinned Java regex semantics exactly).
// ---------------------------------------------------------------------------------------------

/** parseResults: result-link anchors paired index-wise with their snippet cells. */
std::vector<WebResult> parse_results(std::string_view html, std::size_t max_results);

/** stripHtml: `<[^>]*>` removal, then the seven entity replacements IN ORDER (no trim). */
std::string strip_html(std::string_view text);

/** extractUrl: the uddg redirect param (URLDecoder), `//` → `https:`, raw otherwise. */
std::string extract_url(std::string_view href);

// ---------------------------------------------------------------------------------------------
// The search loop — DuckDuckGoScraper.search with fetchPage replaced by an injected page
// source (the form values query/offset/vqd travel to the transport seam).
// ---------------------------------------------------------------------------------------------

enum class SearchErrorType { Captcha, NetworkError, NoResults };

/** The enum names the traces pin (CAPTCHA | NETWORK_ERROR | NO_RESULTS). */
const char* search_error_type_name(SearchErrorType type);

struct SearchOutcome {
    bool success = false;
    std::vector<WebResult> results;
    SearchErrorType error_type = SearchErrorType::NoResults;
    std::string error_message;
};

struct PageResult {
    bool failed = false;
    std::string html;     // when fetched
    std::string message;  // when failed
};

/** fetchPage's seam: the scenario/test sources return canned pages; real callers fetch. */
using PageSource = std::function<PageResult(std::string_view query, int offset,
                                            const std::string& vqd)>;

/** DuckDuckGoScraper.search(query, maxResults) over the injected source. */
SearchOutcome search(std::string_view query, std::size_t max_results,
                     const PageSource& fetch_page);

}  // namespace agora::core::duckduckgo
