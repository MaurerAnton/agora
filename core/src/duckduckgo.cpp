#include "agora/core/duckduckgo.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::duckduckgo {
namespace {

namespace json = agora::core::json;

// ---------------------------------------------------------------------------------------------
// Character classes of the pinned regexes (Java semantics).
// ---------------------------------------------------------------------------------------------

/** Java `\s`: space, tab, newline, form feed, carriage return, vertical tab. */
bool is_java_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == '\x0B';
}

bool is_quote(char c) { return c == '\'' || c == '"'; }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::size_t hex_value(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::size_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::size_t>(c - 'a' + 10);
    return static_cast<std::size_t>(c - 'A' + 10);
}

// ---------------------------------------------------------------------------------------------
// `href=['"]([^'"]*?)['"]` — the href value inside one anchor tag.
// ---------------------------------------------------------------------------------------------

std::optional<std::string_view> href_value(std::string_view tag) {
    std::size_t pos = 0;
    while ((pos = tag.find("href=", pos)) != std::string_view::npos) {
        const std::size_t quote_pos = pos + 5;
        if (quote_pos < tag.size() && is_quote(tag[quote_pos])) {
            // `[^'"]*?` stops at the FIRST quote character of either kind.
            for (std::size_t i = quote_pos + 1; i < tag.size(); ++i) {
                if (is_quote(tag[i])) return tag.substr(quote_pos + 1, i - quote_pos - 1);
            }
        }
        pos += 5;  // "href=" cannot overlap itself, so the next search starts past it
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// `<a\s[^>]*class=['"]result-link['"][^>]*>` — the result-link anchor prefix, ending after '>'.
// ---------------------------------------------------------------------------------------------

std::size_t match_result_link_prefix(std::string_view html, std::size_t start) {
    if (start + 2 >= html.size()) return std::string_view::npos;
    if (html[start] != '<' || html[start + 1] != 'a') return std::string_view::npos;
    if (!is_java_space(html[start + 2])) return std::string_view::npos;
    const std::size_t segment_begin = start + 3;  // `[^>]*` covers the rest of the line
    const std::size_t gt = html.find('>', segment_begin);
    if (gt == std::string_view::npos) return std::string_view::npos;
    // `class=` + quote + `result-link` + quote must live entirely before the first '>'.
    for (std::size_t p = segment_begin; p + 18 < gt; ++p) {
        if (html.compare(p, 6, "class=") != 0) continue;
        if (!is_quote(html[p + 6])) continue;
        if (html.compare(p + 7, 11, "result-link") != 0) continue;
        if (!is_quote(html[p + 18])) continue;
        return gt + 1;
    }
    return std::string_view::npos;
}

struct TagMatch {
    std::string_view value;
};

struct TextMatch {
    std::string_view capture;
};

/** LINK_TAG_REGEX.findAll: non-overlapping matches, resume after each match end. */
std::vector<TagMatch> find_link_tags(std::string_view html) {
    std::vector<TagMatch> matches;
    std::size_t pos = 0;
    while (pos < html.size()) {
        const std::size_t found = html.find("<a", pos);
        if (found == std::string_view::npos) break;
        const std::size_t end = match_result_link_prefix(html, found);
        if (end != std::string_view::npos) {
            matches.push_back(TagMatch{html.substr(found, end - found)});
            pos = end;
        } else {
            pos = found + 1;
        }
    }
    return matches;
}

/** LINK_TEXT_REGEX.findAll: the same prefix, capture lazy until the first `</a>`. */
std::vector<TextMatch> find_link_texts(std::string_view html) {
    std::vector<TextMatch> matches;
    std::size_t pos = 0;
    while (pos < html.size()) {
        const std::size_t found = html.find("<a", pos);
        if (found == std::string_view::npos) break;
        const std::size_t end = match_result_link_prefix(html, found);
        if (end == std::string_view::npos) {
            pos = found + 1;
            continue;
        }
        const std::size_t close = html.find("</a>", end);
        if (close == std::string_view::npos) {
            pos = found + 1;  // no match at this start; the engine retries one char later
            continue;
        }
        matches.push_back(TextMatch{html.substr(end, close - end)});
        pos = close + 4;
    }
    return matches;
}

// ---------------------------------------------------------------------------------------------
// `<td[^>]+class=['"]result-snippet['"][^>]*>([\s\S]*?)</td>`
// ---------------------------------------------------------------------------------------------

struct SnippetMatch {
    std::string_view capture;
};

std::vector<SnippetMatch> find_snippets(std::string_view html) {
    std::vector<SnippetMatch> matches;
    std::size_t pos = 0;
    while (pos < html.size()) {
        const std::size_t found = html.find("<td", pos);
        if (found == std::string_view::npos) break;
        const std::size_t gt = html.find('>', found + 3);
        bool matched = false;
        std::size_t close = std::string_view::npos;
        if (gt != std::string_view::npos) {
            // `[^>]+` consumes at least one char, so the class token starts at >= found + 4.
            for (std::size_t p = found + 4; p + 21 < gt; ++p) {
                if (html.compare(p, 6, "class=") != 0) continue;
                if (!is_quote(html[p + 6])) continue;
                if (html.compare(p + 7, 14, "result-snippet") != 0) continue;
                if (!is_quote(html[p + 21])) continue;
                close = html.find("</td>", gt + 1);
                if (close == std::string_view::npos) break;
                matches.push_back(SnippetMatch{html.substr(gt + 1, close - (gt + 1))});
                matched = true;
                break;
            }
        }
        pos = matched ? close + 5 : found + 1;
    }
    return matches;
}

// ---------------------------------------------------------------------------------------------
// The companion regexes: captcha, vqd, offset.
// ---------------------------------------------------------------------------------------------

/** CAPTCHA_REGEX: anomaly-modal | challenge-form | Unfortunately.*bots (no DOTALL). */
bool captcha_present(std::string_view html) {
    if (html.find("anomaly-modal") != std::string_view::npos) return true;
    if (html.find("challenge-form") != std::string_view::npos) return true;
    std::size_t pos = 0;
    while ((pos = html.find("Unfortunately", pos)) != std::string_view::npos) {
        const std::size_t line_end_result = html.find('\n', pos);
        const std::size_t line_end =
            line_end_result == std::string_view::npos ? html.size() : line_end_result;
        const std::size_t bots = html.find("bots", pos + 12);
        if (bots != std::string_view::npos && bots < line_end) return true;
        pos += 12;
    }
    return false;
}

/** VQD_REGEX: name="vqd"\s+value="([^"]*)" */
std::optional<std::string> vqd_token(std::string_view html) {
    constexpr std::string_view kPrefix = "name=\"vqd\"";
    std::size_t pos = 0;
    while ((pos = html.find(kPrefix, pos)) != std::string_view::npos) {
        std::size_t i = pos + kPrefix.size();
        const std::size_t ws_start = i;
        while (i < html.size() && is_java_space(html[i])) ++i;
        if (i > ws_start && html.compare(i, 7, "value=\"") == 0) {
            const std::size_t close = html.find('"', i + 7);
            if (close != std::string_view::npos) {
                return std::string(html.substr(i + 7, close - i - 7));
            }
        }
        pos += kPrefix.size();
    }
    return std::nullopt;
}

/** OFFSET_REGEX: name="s"\s+value="(\d+)" with String.toIntOrNull() semantics — the FIRST
 *  pattern match decides (overflow → null, no further scanning). */
std::optional<int> offset_value(std::string_view html) {
    constexpr std::string_view kPrefix = "name=\"s\"";
    std::size_t pos = 0;
    while ((pos = html.find(kPrefix, pos)) != std::string_view::npos) {
        std::size_t i = pos + kPrefix.size();
        const std::size_t ws_start = i;
        while (i < html.size() && is_java_space(html[i])) ++i;
        if (i > ws_start && html.compare(i, 7, "value=\"") == 0) {
            std::size_t end = i + 7;
            const std::size_t digits_start = end;
            std::uint64_t accumulator = 0;
            bool overflow = false;
            while (end < html.size() && is_digit(html[end])) {
                if (!overflow) {
                    accumulator = accumulator * 10 +
                                  static_cast<std::uint64_t>(html[end] - '0');
                    if (accumulator > 2147483647ull) {
                        overflow = true;
                        accumulator = 0;
                    }
                }
                ++end;
            }
            if (end == digits_start) {
                pos += kPrefix.size();  // no digits: not a pattern match, try the next one
                continue;
            }
            if (end >= html.size() || html[end] != '"') {
                pos += kPrefix.size();  // not a pattern match either
                continue;
            }
            if (overflow) return std::nullopt;  // toIntOrNull(): the match exists, the value doesn't
            return static_cast<int>(accumulator);
        }
        pos += kPrefix.size();
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// URLDecoder.decode(value, "UTF-8"): '+' → space, %-escapes to bytes (byte-preserving here,
// our strings already are UTF-8). Invalid escapes are out of the pinned corpus and throw, like
// the JDK throws IllegalArgumentException.
// ---------------------------------------------------------------------------------------------

std::string java_url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+') {
            out.push_back(' ');
            continue;
        }
        if (c == '%') {
            if (i + 2 >= text.size() || !is_hex_digit(text[i + 1]) || !is_hex_digit(text[i + 2])) {
                throw std::invalid_argument(
                    "URLDecoder: Incomplete trailing escape (%) pattern");
            }
            out.push_back(static_cast<char>(hex_value(text[i + 1]) * 16 +
                                            hex_value(text[i + 2])));
            i += 2;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::string out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = text.find(from, pos);
        if (found == std::string::npos) {
            out += text.substr(pos);
            return out;
        }
        out += text.substr(pos, found - pos);
        out += to;
        pos = found + from.size();
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The pinned parsing pieces.
// ---------------------------------------------------------------------------------------------

std::string strip_html(std::string_view text) {
    // HTML_TAG_REGEX = `<[^>]*>` — replace all (a '<' with no '>' anywhere after it and no
    // later '<' with one simply survives, which the loop handles by flushing the tail).
    std::string out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t lt = text.find('<', pos);
        if (lt == std::string_view::npos) {
            out += text.substr(pos);
            break;
        }
        const std::size_t gt = text.find('>', lt + 1);
        if (gt == std::string_view::npos) {
            out += text.substr(pos);
            break;
        }
        out += text.substr(pos, lt - pos);
        pos = gt + 1;
    }
    // The seven entity replacements, IN ORDER (double unescaping is pinned: `&amp;lt;` → `<`).
    out = replace_all(std::move(out), "&amp;", "&");
    out = replace_all(std::move(out), "&lt;", "<");
    out = replace_all(std::move(out), "&gt;", ">");
    out = replace_all(std::move(out), "&quot;", "\"");
    out = replace_all(std::move(out), "&#x27;", "'");
    out = replace_all(std::move(out), "&#39;", "'");
    out = replace_all(std::move(out), "&nbsp;", " ");
    return out;
}

std::string extract_url(std::string_view href) {
    // uddg=([^&]+)
    std::size_t pos = 0;
    while ((pos = href.find("uddg=", pos)) != std::string_view::npos) {
        std::size_t end = pos + 5;
        const std::size_t start = end;
        while (end < href.size() && href[end] != '&') ++end;
        if (end > start) {
            return java_url_decode(href.substr(start, end - start));
        }
        pos = start;
    }
    if (href.rfind("//", 0) == 0) return "https:" + std::string(href);
    return std::string(href);
}

std::vector<WebResult> parse_results(std::string_view html, std::size_t max_results) {
    const std::vector<TagMatch> link_tags = find_link_tags(html);
    const std::vector<TextMatch> link_texts = find_link_texts(html);
    const std::vector<SnippetMatch> snippets = find_snippets(html);

    std::vector<WebResult> results;
    for (std::size_t i = 0; i < link_texts.size(); ++i) {
        if (results.size() >= max_results) break;
        if (i >= link_tags.size()) continue;  // linkTags.getOrNull(i) ?: continue
        const std::optional<std::string_view> href = href_value(link_tags[i].value);
        if (!href) continue;
        const std::string title = json::trim(strip_html(link_texts[i].capture));
        const std::string snippet =
            i < snippets.size() ? json::trim(strip_html(snippets[i].capture)) : std::string();
        const std::string url = extract_url(*href);
        if (!json::is_blank(url) && !json::is_blank(title)) {
            results.push_back(WebResult{title, url, snippet});
        }
    }
    return results;
}

// ---------------------------------------------------------------------------------------------
// The search loop.
// ---------------------------------------------------------------------------------------------

const char* search_error_type_name(SearchErrorType type) {
    switch (type) {
        case SearchErrorType::Captcha: return "CAPTCHA";
        case SearchErrorType::NetworkError: return "NETWORK_ERROR";
        case SearchErrorType::NoResults: return "NO_RESULTS";
    }
    return "NO_RESULTS";
}

namespace {

constexpr int kMaxPages = 5;
constexpr std::size_t kPageSize = 10;

SearchOutcome failure(SearchErrorType type, std::string message) {
    SearchOutcome outcome;
    outcome.success = false;
    outcome.error_type = type;
    outcome.error_message = std::move(message);
    return outcome;
}

}  // namespace

SearchOutcome search(std::string_view query, std::size_t max_results,
                     const PageSource& fetch_page) {
    std::vector<WebResult> all_results;
    std::set<std::string> seen_urls;
    int offset = 0;
    std::string vqd;

    for (int page = 0; page < kMaxPages; ++page) {
        if (static_cast<int>(all_results.size()) >= static_cast<int>(max_results)) break;

        const PageResult page_result = fetch_page(query, offset, vqd);
        if (page_result.failed) {
            if (all_results.empty()) {
                return failure(SearchErrorType::NetworkError, page_result.message);
            }
            // Partial results — return what we have rather than failing.
            break;
        }
        const std::string& html = page_result.html;

        if (captcha_present(html)) {
            if (all_results.empty()) {
                return failure(SearchErrorType::Captcha,
                               "DuckDuckGo bot detection triggered. Try again later or use a "
                               "different search provider.");
            }
            // Partial results are better than nothing — return what we have.
            break;
        }

        // Update vqd for subsequent pages (first page provides the token).
        if (page == 0) vqd = vqd_token(html).value_or("");

        const std::vector<WebResult> page_results = parse_results(html, kPageSize);
        int new_in_this_page = 0;
        for (const WebResult& result : page_results) {
            if (static_cast<int>(all_results.size()) >= static_cast<int>(max_results)) break;
            if (seen_urls.insert(result.url).second) {
                all_results.push_back(result);
                ++new_in_this_page;
            }
        }

        if (new_in_this_page == 0) break;  // No new unique results.
        if (vqd.empty()) break;           // No Next Page button.

        const std::optional<int> next_offset = offset_value(html);
        if (!next_offset) break;          // Malformed pagination form.
        offset = *next_offset;
    }

    if (all_results.empty()) {
        return failure(SearchErrorType::NoResults,
                       "DuckDuckGo returned no results for this query.");
    }
    SearchOutcome outcome;
    outcome.success = true;
    outcome.results = std::move(all_results);
    return outcome;
}

}  // namespace agora::core::duckduckgo
