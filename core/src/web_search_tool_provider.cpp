#include "agora/core/web_search_tool_provider.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <regex>
#include <stdexcept>
#include <utility>

#include "agora/core/json.hpp"
#include "agora/core/utf16.hpp"

namespace agora::core::websearch {

namespace {

namespace json = agora::core::json;
namespace duck = agora::core::duckduckgo;
using shell_provider::GenerationContext;

// ---- JSON builders (kotlinx-compatible rendering through json::to_kotlin_string) -------------

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_literal(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

json::Value json_bool(bool flag) { return json_literal(flag ? "true" : "false"); }

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

json::Value json_array(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

std::string render(const json::Value& value) { return json::to_kotlin_string(value); }

// ---- Kotlin accessors --------------------------------------------------------------------------

/** Kotlin Map<String, JsonElement> lookup: kotlinx decodes duplicates last-wins. */
const json::Value* map_get(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

/** `(element as? JsonPrimitive)?.content` — JsonNull IS a JsonPrimitive in kotlinx. */
std::optional<std::string> primitive_content(const json::Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == json::Kind::String || value->kind == json::Kind::Literal) return value->text;
    return std::nullopt;  // JsonObject/JsonArray fail the `as? JsonPrimitive` cast
}

/** `.jsonArray` — kotlinx throws "is not a JsonArray". */
const json::Value& require_array(const json::Value* value) {
    if (value == nullptr || value->kind != json::Kind::Array) {
        throw std::runtime_error("Element is not a JsonArray");
    }
    return *value;
}

/** `.jsonObject` — kotlinx throws "is not a JsonObject". */
const json::Value& require_object(const json::Value* value) {
    if (value == nullptr || value->kind != json::Kind::Object) {
        throw std::runtime_error("Element is not a JsonObject");
    }
    return *value;
}

json::Value parse_json(const std::string& source) {
    const json::ParseResult parsed = json::parse(source);
    if (!parsed.value) {
        throw std::runtime_error("Unexpected JSON token at offset " +
                                 std::to_string(parsed.error_position) + ": " + parsed.error);
    }
    return *parsed.value;
}

/** Kotlin String.toIntOrNull(): optional sign, ASCII digits, non-empty, Int range. */
std::optional<std::int32_t> kotlin_int(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::size_t at = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        at = 1;
    }
    if (at == text.size()) return std::nullopt;
    std::int64_t value = 0;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
        if (value > 2147483648LL) return std::nullopt;  // Int range (sign handled below)
    }
    if (negative) {
        if (value > 2147483648LL) return std::nullopt;
        return static_cast<std::int32_t>(-value);
    }
    if (value > 2147483647LL) return std::nullopt;
    return static_cast<std::int32_t>(value);
}

/** Kotlin String.toFloatOrNull(): full-string float, JVM specials accepted. */
std::optional<float> kotlin_float(std::string_view text) {
    if (text.empty()) return std::nullopt;
    // Kotlin/JVM parse "NaN" and "±Infinity" (case-insensitively).
    std::string lowered(text);
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (lowered == "nan") return std::numeric_limits<float>::quiet_NaN();
    if (lowered == "infinity" || lowered == "+infinity") {
        return std::numeric_limits<float>::infinity();
    }
    if (lowered == "-infinity") return -std::numeric_limits<float>::infinity();
    std::string_view view = text;
    if (!view.empty() && view.front() == '+') view.remove_prefix(1);  // from_chars has no '+'
    if (view.empty()) return std::nullopt;
    float value = 0.0f;
    const auto [end, error] =
        std::from_chars(view.data(), view.data() + view.size(), value);
    if (error != std::errc{} || end != view.data() + view.size()) return std::nullopt;
    return value;
}

std::int32_t clamp_i32(std::int32_t value, std::int32_t low, std::int32_t high) {
    return value < low ? low : (value > high ? high : value);
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

/**
 * Java Float.toString (what kotlinx writes for a JsonPrimitive(Float)): shortest round-trip
 * digits, decimal for -3 ≤ exponent ≤ 6, computerized scientific otherwise, always at least
 * one fraction digit in decimal form and a ".0" mantissa in scientific form.
 */
std::string kotlin_float_string(float value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::scientific);
    if (result.ec != std::errc{}) throw std::runtime_error("float formatting failed");
    std::string scientific(buffer, result.ptr);
    // Split "[-]d[.ddd]e[+-]X" into sign, digits and exponent.
    std::size_t at = 0;
    bool negative = false;
    if (scientific[at] == '-') {
        negative = true;
        ++at;
    }
    std::string digits;
    while (at < scientific.size() && scientific[at] != 'e') {
        if (scientific[at] != '.') digits.push_back(scientific[at]);
        ++at;
    }
    const int exponent = std::stoi(scientific.substr(at + 1));
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();  // to_chars keeps 1..N digits

    const std::string sign = negative ? "-" : "";
    if (exponent < -3 || exponent >= 7) {
        std::string mantissa = std::string(1, digits[0]);
        if (digits.size() > 1) {
            mantissa += ".";
            mantissa += digits.substr(1);
        } else {
            mantissa += ".0";
        }
        std::string out = sign + mantissa + "E";
        if (exponent < 0) out += "-";
        out += std::to_string(exponent < 0 ? -exponent : exponent);
        return out;
    }
    std::string out = sign;
    if (exponent >= 0) {
        if (static_cast<std::size_t>(exponent) + 1 <= digits.size()) {
            const std::string fraction = digits.substr(static_cast<std::size_t>(exponent) + 1);
            out += digits.substr(0, static_cast<std::size_t>(exponent) + 1);
            out += ".";
            out += fraction.empty() ? "0" : fraction;
        } else {
            out += digits;
            out.append(static_cast<std::size_t>(exponent) + 1 - digits.size(), '0');
            out += ".0";
        }
        return out;
    }
    out += "0.";
    out.append(static_cast<std::size_t>(-exponent - 1), '0');
    out += digits;
    return out;
}

// ---- Error / result envelopes (Kotlin buildJsonObject insertion order) --------------------------

json::Value web_search_error(std::string_view query, std::string_view error,
                             std::optional<std::string> message = std::nullopt,
                             std::optional<std::string> provider = std::nullopt) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("type", json_string("web_search"));
    members.emplace_back("query", json_string(std::string(query)));
    members.emplace_back("error", json_string(std::string(error)));
    if (message.has_value()) members.emplace_back("message", json_string(*message));
    if (provider.has_value()) members.emplace_back("provider", json_string(*provider));
    return json_object(std::move(members));
}

json::Value web_search_results(const std::string& query, std::vector<json::Value> results) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("type", json_string("web_search"));
    members.emplace_back("query", json_string(query));
    members.emplace_back("results", json_array(std::move(results)));
    return json_object(std::move(members));
}

std::string web_search_error_json(std::string_view query, std::string_view error,
                                  std::optional<std::string> message = std::nullopt,
                                  std::optional<std::string> provider = std::nullopt) {
    return render(web_search_error(query, error, std::move(message), std::move(provider)));
}

std::string no_results_json(const std::string& query) {
    return render(web_search_error(query, "no_results"));
}

}  // namespace

// ---- Pure helpers -------------------------------------------------------------------------------

std::string normalize_web_search_provider(const std::optional<std::string>& provider) {
    if (!provider.has_value()) return "duckduckgo";
    std::string trimmed = json::trim(*provider);
    for (char& c : trimmed) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    static constexpr const char* kProviders[] = {"duckduckgo", "brave", "kagi",
                                                 "serper",     "tavily", "searxng"};
    for (const char* candidate : kProviders) {
        if (trimmed == candidate) return trimmed;
    }
    return "duckduckgo";
}

std::string web_search_provider_display_name(const std::string& provider) {
    if (provider == "kagi") return "Kagi";
    if (provider == "serper") return "Serper";
    if (provider == "tavily") return "Tavily";
    if (provider == "searxng") return "SearXNG";
    if (provider == "duckduckgo") return "DuckDuckGo";
    return "Brave Search";
}

std::string url_encode_form(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const unsigned char byte : text) {
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '.' || byte == '-' ||
                                byte == '*' || byte == '_';
        if (byte == ' ') {
            out += '+';
        } else if (unreserved) {
            out += static_cast<char>(byte);
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
        }
    }
    return out;
}

std::string searxng_search_url(const std::string& configured_base_url, const std::string& query) {
    std::string base = configured_base_url.empty() ? "https://searx.be" : configured_base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/search?q=" + url_encode_form(query) + "&format=json";
}

std::string kagi_search_request_body(const std::string& query, std::int32_t num_results) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("query", json_string(query));
    members.emplace_back("workflow", json_string("search"));
    members.emplace_back("limit", json_literal(std::to_string(clamp_i32(num_results, 1, 10))));
    return render(json_object(std::move(members)));
}

std::string normalize_kagi_search_response(const std::string& response_body,
                                           const std::string& query, std::int32_t num_results) {
    // Kotlin: Json.parseToJsonElement throws on malformed input (the caller's catch folds it
    // into "search_error"); `as? JsonObject` only guards the kind.
    const json::Value root = parse_json(response_body);
    const json::Value* data = map_get(root, "data");
    const json::Value* search = data != nullptr ? map_get(*data, "search") : nullptr;
    if (root.kind != json::Kind::Object || data == nullptr || data->kind != json::Kind::Object ||
        search == nullptr || search->kind != json::Kind::Array) {
        return render(web_search_error(query, "no_results"));
    }

    const std::int32_t limit = clamp_i32(num_results, 1, 10);
    std::vector<json::Value> results;
    for (const json::Value& element : search->elements) {
        if (static_cast<std::int32_t>(results.size()) >= limit) break;
        if (element.kind != json::Kind::Object) continue;
        const std::optional<std::string> url = primitive_content(map_get(element, "url"));
        if (!url.has_value() || json::is_blank(*url)) continue;
        std::vector<std::pair<std::string, json::Value>> item;
        item.emplace_back("title",
                          json_string(primitive_content(map_get(element, "title")).value_or("")));
        item.emplace_back("url", json_string(*url));
        item.emplace_back("description",
                          json_string(primitive_content(map_get(element, "snippet")).value_or("")));
        results.push_back(json_object(std::move(item)));
    }
    if (results.empty()) return render(web_search_error(query, "no_results"));
    return render(web_search_results(query, std::move(results)));
}

std::string html_to_readable_text(const std::string& raw_html, const HtmlToText& decode) {
    // Kotlin: rawHtml.take(MAX_WEB_FETCH_HTML_LENGTH) — UTF-16 units, like String.take.
    std::string stripped = json::take_utf16_units(raw_html, static_cast<std::size_t>(
                                                                kMaxWebFetchHtmlLength));
    // Comments, then non-content/chrome blocks (case-insensitive, backreference to the tag name).
    const std::regex comments("<!--[\\s\\S]*?-->");
    stripped = std::regex_replace(stripped, comments, " ");
    const std::regex non_content(
        "<(script|style|noscript|svg|head)\\b[^>]*>[\\s\\S]*?</\\1>",
        std::regex_constants::icase | std::regex_constants::ECMAScript);
    stripped = std::regex_replace(stripped, non_content, " ");
    const std::regex chrome(
        "<(nav|header|footer|aside)\\b[^>]*>[\\s\\S]*?</\\1>",
        std::regex_constants::icase | std::regex_constants::ECMAScript);
    stripped = std::regex_replace(stripped, chrome, " ");

    std::string text = decode(stripped);

    const std::regex intra_line("[ \\t\\x0B\\x0C\\r]+");
    text = std::regex_replace(text, intra_line, " ");
    const std::regex around_newline(" *\\n *");
    text = std::regex_replace(text, around_newline, "\n");
    const std::regex blank_runs("\\n{3,}");
    text = std::regex_replace(text, blank_runs, "\n\n");
    return json::trim(text);
}

// ---- Provider ------------------------------------------------------------------------------------

WebSearchToolProvider::WebSearchToolProvider(Transport& transport, HtmlToText html_to_text)
    : transport_(transport), html_to_text_(std::move(html_to_text)) {}

bool WebSearchToolProvider::handles(const std::string& name) const {
    return name == "web_search" || name == "web_fetch";
}

std::vector<openai::ToolDefinition> WebSearchToolProvider::definitions(
    const GenerationContext& ctx) const {
    if (!ctx.web_search_enabled) return {};
    std::vector<openai::ToolDefinition> definitions;
    {
        openai::ToolFunction function;
        function.name = "web_search";
        function.description =
            "Search the web for current information. Use this to find facts, news, or data not "
            "in your training set.";
        openai::ToolProperty query;
        query.type = "string";
        query.description = "The search query to execute.";
        openai::ToolProperty num_results;
        num_results.type = "integer";
        num_results.description = "Number of results to return (1-10, default 5).";
        function.parameters.properties = {
            {"query", std::move(query)},
            {"num_results", std::move(num_results)},
        };
        function.parameters.required = {"query"};
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    {
        openai::ToolFunction function;
        function.name = "web_fetch";
        function.description =
            "Fetch and read the full text content of a web page. Use this after web_search when "
            "you need more detail from a specific page.";
        openai::ToolProperty url;
        url.type = "string";
        url.description = "The URL of the page to fetch.";
        openai::ToolProperty max_chars;
        max_chars.type = "integer";
        max_chars.description =
            "Maximum characters of text to return (default 8000, max 100000). If the result has "
            "\"truncated\": true, call again with a larger maxChars to get more.";
        function.parameters.properties = {
            {"url", std::move(url)},
            {"maxChars", std::move(max_chars)},
        };
        function.parameters.required = {"url"};
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    return definitions;
}

std::string WebSearchToolProvider::execute(const std::string& name, const std::string& arguments,
                                           const GenerationContext& ctx) {
    if (name == "web_search") return execute_web_search(arguments, ctx);
    if (name == "web_fetch") return execute_web_fetch(arguments, ctx);
    return "Unknown tool: " + name;
}

std::string WebSearchToolProvider::execute_web_search(const std::string& arguments,
                                                      const GenerationContext& ctx) {
    const std::string args_str = arguments.empty() ? "{}" : arguments;
    // Kotlin Json.decodeFromString<Map<String, JsonElement>> — a non-object throws here,
    // outside the provider's try (it propagates as a tool failure, like kotlinx does).
    const json::Value args = parse_json(args_str);
    if (args.kind != json::Kind::Object) {
        throw std::runtime_error("Expected JSON object");
    }

    const std::optional<std::string> query =
        primitive_content(map_get(args, "query"));  // `as? JsonPrimitive)?.content`
    if (!query.has_value()) {
        return render(json_object({{"type", json_string("web_search")},
                                   {"error", json_string("no_query")}}));
    }
    const std::optional<std::string> num_results_arg =
        primitive_content(map_get(args, "num_results"));
    std::int32_t num_results = ctx.web_search_num_results;
    if (num_results_arg.has_value()) {
        if (const std::optional<std::int32_t> parsed = kotlin_int(*num_results_arg)) {
            num_results = *parsed;
        }
    }
    num_results = clamp_i32(num_results, 1, 10);
    const std::string provider = normalize_web_search_provider(ctx.web_search_provider);

    try {
        // DuckDuckGo is a scraper, not an API — handle it separately.
        if (provider == "duckduckgo") {
            const duck::SearchOutcome outcome =
                transport_.search_duckduckgo(*query, num_results);
            if (!outcome.success) {
                std::vector<std::pair<std::string, json::Value>> members;
                members.emplace_back("type", json_string("web_search"));
                members.emplace_back("query", json_string(*query));
                members.emplace_back("error",
                                     json_string(lower_ascii(duck::search_error_type_name(
                                         outcome.error_type))));
                members.emplace_back("message", json_string(outcome.error_message));
                return render(json_object(std::move(members)));
            }
            std::vector<json::Value> results;
            for (const duck::WebResult& result : outcome.results) {
                std::vector<std::pair<std::string, json::Value>> item;
                item.emplace_back("title", json_string(result.title));
                item.emplace_back("url", json_string(result.url));
                item.emplace_back("description", json_string(result.snippet));
                results.push_back(json_object(std::move(item)));
            }
            return render(web_search_results(*query, std::move(results)));
        }

        const auto key_it = ctx.web_search_api_keys.find(provider);
        const std::string api_key = key_it == ctx.web_search_api_keys.end() ? "" : key_it->second;
        if (provider != "searxng" && json::is_blank(api_key)) {
            return web_search_error_json(*query, "no_api_key", std::nullopt,
                                         web_search_provider_display_name(provider));
        }

        std::optional<std::string> body;
        if (provider == "kagi") {
            body = transport_.post_json(
                "https://kagi.com/api/v1/search", kagi_search_request_body(*query, num_results),
                {{"Accept", "application/json"}, {"Authorization", "Bearer " + api_key}},
                kNetworkToolTimeoutMs);
        } else if (provider == "serper") {
            std::vector<std::pair<std::string, json::Value>> members;
            members.emplace_back("q", json_string(*query));
            members.emplace_back("num", json_literal(std::to_string(num_results)));
            body = transport_.post_json("https://google.serper.dev/search",
                                        render(json_object(std::move(members))),
                                        {{"X-API-KEY", api_key}}, kNetworkToolTimeoutMs);
        } else if (provider == "tavily") {
            std::vector<std::pair<std::string, json::Value>> members;
            members.emplace_back("api_key", json_string(api_key));
            members.emplace_back("query", json_string(*query));
            members.emplace_back("max_results", json_literal(std::to_string(num_results)));
            members.emplace_back("search_depth", json_string("advanced"));
            members.emplace_back("include_answer", json_bool(true));
            body = transport_.post_json("https://api.tavily.com/search",
                                        render(json_object(std::move(members))), {},
                                        kNetworkToolTimeoutMs);
        } else if (provider == "searxng") {
            body = transport_.get_text(searxng_search_url(ctx.web_search_base_url, *query),
                                       {{"User-Agent", kWebFetchUserAgent}},
                                       kNetworkToolTimeoutMs);
        } else {
            // Brave Search (the normalized default for unknown-but-configured providers).
            body = transport_.get_text(
                "https://api.search.brave.com/res/v1/web/search?q=" + url_encode_form(*query) +
                    "&count=" + std::to_string(num_results),
                {{"Accept", "application/json"}, {"X-Subscription-Token", api_key}},
                kNetworkToolTimeoutMs);
        }
        if (!body.has_value()) {
            return web_search_error_json(*query, "no_response");
        }

        if (provider == "kagi") {
            return normalize_kagi_search_response(*body, *query, num_results);
        }

        const json::Value json = parse_json(*body);
        if (json.kind != json::Kind::Object) {
            throw std::runtime_error("Expected JSON object");
        }

        if (provider == "tavily") {
            const json::Value* results_field = map_get(json, "results");
            if (results_field == nullptr) return no_results_json(*query);
            const json::Value& results_array = require_array(results_field);
            if (results_array.elements.empty()) return no_results_json(*query);
            const std::optional<std::string> answer =
                primitive_content(map_get(json, "answer"));
            std::vector<json::Value> results;
            for (const json::Value& element : results_array.elements) {
                const json::Value& obj = require_object(&element);
                std::vector<std::pair<std::string, json::Value>> item;
                item.emplace_back("title",
                                  json_string(primitive_content(map_get(obj, "title")).value_or("")));
                item.emplace_back("url",
                                  json_string(primitive_content(map_get(obj, "url")).value_or("")));
                item.emplace_back(
                    "content",
                    json_string(primitive_content(map_get(obj, "content")).value_or("")));
                if (const std::optional<std::string> score_text =
                        primitive_content(map_get(obj, "score"))) {
                    if (const std::optional<float> score = kotlin_float(*score_text)) {
                        item.emplace_back("score", json_literal(kotlin_float_string(*score)));
                    }
                }
                results.push_back(json_object(std::move(item)));
            }
            std::vector<std::pair<std::string, json::Value>> members;
            members.emplace_back("type", json_string("web_search"));
            members.emplace_back("query", json_string(*query));
            if (answer.has_value() && !json::is_blank(*answer)) {
                members.emplace_back("answer", json_string(*answer));
            }
            members.emplace_back("results", json_array(std::move(results)));
            return render(json_object(std::move(members)));
        }

        // Generic normalizer: organic → web.results → results.
        const json::Value* results_field = nullptr;
        if (map_get(json, "organic") != nullptr) {
            results_field = map_get(json, "organic");
        } else if (map_get(json, "web") != nullptr) {
            const json::Value& web = require_object(map_get(json, "web"));
            results_field = map_get(web, "results");
        } else if (map_get(json, "results") != nullptr) {
            results_field = map_get(json, "results");
        }
        if (results_field == nullptr) return no_results_json(*query);
        const json::Value& results_array = require_array(results_field);
        if (results_array.elements.empty()) return no_results_json(*query);

        std::vector<json::Value> results;
        for (const json::Value& element : results_array.elements) {
            const json::Value& obj = require_object(&element);
            std::vector<std::pair<std::string, json::Value>> item;
            item.emplace_back("title",
                              json_string(primitive_content(map_get(obj, "title")).value_or("")));
            // Kotlin: (obj["link"] as? JsonPrimitive)?.content ?: (obj["url"] …) — a present
            // primitive (even blank) wins; only a missing/non-primitive link falls through.
            std::optional<std::string> url = primitive_content(map_get(obj, "link"));
            if (!url.has_value()) url = primitive_content(map_get(obj, "url"));
            item.emplace_back("url", json_string(url.value_or("")));
            std::optional<std::string> description = primitive_content(map_get(obj, "snippet"));
            if (!description.has_value()) description = primitive_content(map_get(obj, "content"));
            if (!description.has_value()) description = primitive_content(map_get(obj, "description"));
            item.emplace_back("description", json_string(description.value_or("")));
            results.push_back(json_object(std::move(item)));
        }
        return render(web_search_results(*query, std::move(results)));
    } catch (const std::exception& error) {
        return web_search_error_json(*query, "search_error", error.what());
    }
}

std::string WebSearchToolProvider::execute_web_fetch(const std::string& arguments,
                                                     const GenerationContext& /*ctx*/) {
    const std::string args_str = arguments.empty() ? "{}" : arguments;
    const json::Value args = parse_json(args_str);
    if (args.kind != json::Kind::Object) {
        throw std::runtime_error("Expected JSON object");
    }

    const std::optional<std::string> url = primitive_content(map_get(args, "url"));
    if (!url.has_value()) {
        return render(json_object({{"type", json_string("web_fetch")},
                                   {"error", json_string("no_url")}}));
    }
    std::int32_t max_chars = 8000;
    if (const std::optional<std::string> max_chars_arg =
            primitive_content(map_get(args, "maxChars"))) {
        if (const std::optional<std::int32_t> parsed = kotlin_int(*max_chars_arg)) {
            max_chars = *parsed;
        }
    }
    max_chars = clamp_i32(max_chars, 1, 100'000);

    try {
        const std::optional<std::string> html = transport_.get_text(
            *url, {{"User-Agent", kWebFetchUserAgent},
                   {"Accept", "text/html,application/xhtml+xml,*/*"}},
            kNetworkToolTimeoutMs);
        if (!html.has_value()) {
            return render(json_object({{"type", json_string("web_fetch")},
                                       {"url", json_string(*url)},
                                       {"error", json_string("no_response")}}));
        }
        const std::string full_text = html_to_readable_text(*html, html_to_text_);
        const std::string text =
            json::take_utf16_units(full_text, static_cast<std::size_t>(max_chars));
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("type", json_string("web_fetch"));
        members.emplace_back("url", json_string(*url));
        members.emplace_back("text", json_string(text));
        members.emplace_back("truncated",
                             json_bool(json::utf16_length(full_text) > json::utf16_length(text)));
        members.emplace_back("totalChars",
                             json_literal(std::to_string(json::utf16_length(full_text))));
        return render(json_object(std::move(members)));
    } catch (const std::exception& error) {
        return render(json_object({{"type", json_string("web_fetch")},
                                   {"url", json_string(*url)},
                                   {"error", json_string("fetch_error")},
                                   {"message", json_string(error.what())}}));
    }
}

}  // namespace agora::core::websearch