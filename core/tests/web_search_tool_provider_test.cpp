// Unit tests for the WebSearchToolProvider port: definitions, argument coercion, every
// provider's request shape, the normalized result envelopes and the HTML→text pipeline
// (the HtmlCompat half arrives through the injected decode seam).

#include "agora/core/web_search_tool_provider.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"

namespace websearch = agora::core::websearch;
namespace shell_provider = agora::core::shell_provider;
namespace duck = agora::core::duckduckgo;
using websearch::WebSearchToolProvider;

namespace {

namespace json = agora::core::json;

struct ScriptedTransport final : websearch::Transport {
    struct Call {
        std::string kind;  // "post" | "get" | "ddg"
        std::string url;
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
        std::int64_t timeout_ms = 0;
        std::string ddg_query;
        std::int32_t ddg_num = 0;
    };

    std::vector<Call> calls;
    std::optional<std::string> post_result;
    std::optional<std::string> get_result;
    bool throw_on_call = false;
    std::string throw_message = "boom";
    duck::SearchOutcome ddg_outcome;

    ScriptedTransport() { ddg_outcome.success = true; }

    std::optional<std::string> post_json(const std::string& url, const std::string& body,
                                         const std::vector<std::pair<std::string, std::string>>& headers,
                                         std::int64_t call_timeout_ms) override {
        calls.push_back(Call{"post", url, body, headers, call_timeout_ms, "", 0});
        if (throw_on_call) throw std::runtime_error(throw_message);
        return post_result;
    }

    std::optional<std::string> get_text(const std::string& url,
                                        const std::vector<std::pair<std::string, std::string>>& headers,
                                        std::int64_t call_timeout_ms) override {
        calls.push_back(Call{"get", url, "", headers, call_timeout_ms, "", 0});
        if (throw_on_call) throw std::runtime_error(throw_message);
        return get_result;
    }

    duck::SearchOutcome search_duckduckgo(const std::string& query,
                                          std::int32_t num_results) override {
        calls.push_back(Call{"ddg", "", "", {}, 0, query, num_results});
        if (throw_on_call) throw std::runtime_error(throw_message);
        return ddg_outcome;
    }

    const Call& last() const { return calls.back(); }

    std::string header_value(const std::string& name) const {
        for (const auto& [key, value] : last().headers) {
            if (key == name) return value;
        }
        return "";
    }
};

/** Identity decoder: the strip passes run in the provider; the seam only decodes what survives. */
websearch::HtmlToText tag_stripping_decoder() {
    return [](const std::string& html) {
        std::string out;
        std::size_t at = 0;
        while (at < html.size()) {
            const std::size_t open = html.find('<', at);
            if (open == std::string::npos) {
                out.append(html, at, html.size() - at);
                break;
            }
            out.append(html, at, open - at);
            const std::size_t close = html.find('>', open);
            if (close == std::string::npos) {
                out.append(html, open, html.size() - open);
                break;
            }
            out += ' ';  // HtmlCompat flattens tags to text boundaries
            at = close + 1;
        }
        // A tiny entity decode so the seam visibly "decodes" (HtmlCompat's job).
        std::string decoded;
        std::size_t pos = 0;
        while (true) {
            const std::size_t amp = out.find('&', pos);
            if (amp == std::string::npos) {
                decoded.append(out, pos, out.size() - pos);
                break;
            }
            decoded.append(out, pos, amp - pos);
            if (out.compare(amp, 5, "&amp;") == 0) {
                decoded += '&';
                pos = amp + 5;
            } else if (out.compare(amp, 6, "&nbsp;") == 0) {
                decoded += ' ';
                pos = amp + 6;
            } else {
                decoded += '&';
                pos = amp + 1;
            }
        }
        return decoded;
    };
}

shell_provider::GenerationContext make_ctx() {
    shell_provider::GenerationContext ctx;
    ctx.web_search_enabled = true;
    ctx.web_search_num_results = 5;
    ctx.web_search_provider = "duckduckgo";
    return ctx;
}

duck::SearchOutcome ddg_success(std::vector<duck::WebResult> results) {
    duck::SearchOutcome outcome;
    outcome.success = true;
    outcome.results = std::move(results);
    return outcome;
}

duck::SearchOutcome ddg_error(duck::SearchErrorType type, std::string message) {
    duck::SearchOutcome outcome;
    outcome.success = false;
    outcome.error_type = type;
    outcome.error_message = std::move(message);
    return outcome;
}

}  // namespace

// ---- Helpers ------------------------------------------------------------------------------------

TEST(web_search_normalize_provider) {
    CHECK_EQ(std::string("duckduckgo"), websearch::normalize_web_search_provider(std::nullopt));
    CHECK_EQ(std::string("duckduckgo"), websearch::normalize_web_search_provider(""));
    CHECK_EQ(std::string("duckduckgo"), websearch::normalize_web_search_provider("  "));
    CHECK_EQ(std::string("duckduckgo"), websearch::normalize_web_search_provider("wibble"));
    CHECK_EQ(std::string("kagi"), websearch::normalize_web_search_provider("kagi"));
    // trim + lowercase
    CHECK_EQ(std::string("searxng"), websearch::normalize_web_search_provider("  SeArXnG "));
    CHECK_EQ(std::string("tavily"), websearch::normalize_web_search_provider("TAVILY"));
    CHECK_EQ(std::string("serper"), websearch::normalize_web_search_provider("serper"));
    CHECK_EQ(std::string("brave"), websearch::normalize_web_search_provider("brave"));
}

TEST(web_search_provider_display_names) {
    CHECK_EQ(std::string("Kagi"), websearch::web_search_provider_display_name("kagi"));
    CHECK_EQ(std::string("Serper"), websearch::web_search_provider_display_name("serper"));
    CHECK_EQ(std::string("Tavily"), websearch::web_search_provider_display_name("tavily"));
    CHECK_EQ(std::string("SearXNG"), websearch::web_search_provider_display_name("searxng"));
    CHECK_EQ(std::string("DuckDuckGo"),
             websearch::web_search_provider_display_name("duckduckgo"));
    CHECK_EQ(std::string("Brave Search"), websearch::web_search_provider_display_name("brave"));
    CHECK_EQ(std::string("Brave Search"), websearch::web_search_provider_display_name("other"));
}

TEST(web_search_url_encode_form) {
    // URLEncoder.encode(text, "UTF-8"): space → '+', unreserved kept, the rest %XX uppercase.
    CHECK_EQ(std::string("hello+world"), websearch::url_encode_form("hello world"));
    CHECK_EQ(std::string("a%26b%3Dc"), websearch::url_encode_form("a&b=c"));
    CHECK_EQ(std::string("h%C3%A9llo"), websearch::url_encode_form("héllo"));
    CHECK_EQ(std::string("safe-*_.019"), websearch::url_encode_form("safe-*_.019"));
    CHECK_EQ(std::string("%2F%3F%23"), websearch::url_encode_form("/?#"));
    CHECK_EQ(std::string(""), websearch::url_encode_form(""));
}

TEST(web_search_searxng_url) {
    CHECK_EQ(std::string("https://searx.be/search?q=a+b&format=json"),
             websearch::searxng_search_url("", "a b"));
    CHECK_EQ(std::string("https://searx.example/search?q=cats&format=json"),
             websearch::searxng_search_url("https://searx.example/", "cats"));
    CHECK_EQ(std::string("https://searx.example/search?q=%C3%BCmlaute&format=json"),
             websearch::searxng_search_url("https://searx.example", "ümlaute"));
}

TEST(web_search_kagi_request_body) {
    CHECK_EQ(std::string(R"({"query":"cats","workflow":"search","limit":5})"),
             websearch::kagi_search_request_body("cats", 5));
    // limit coerces into 1..10 (Kotlin coerceIn).
    CHECK_EQ(std::string(R"({"query":"q","workflow":"search","limit":1})"),
             websearch::kagi_search_request_body("q", 0));
    CHECK_EQ(std::string(R"({"query":"q","workflow":"search","limit":10})"),
             websearch::kagi_search_request_body("q", 15));
}

TEST(web_search_kagi_normalize_results) {
    const std::string body =
        R"({"data":{"search":[{"url":"https://a","title":"A","snippet":"sa"},)"
        R"({"url":"https://b","title":"B","snippet":"sb"},)"
        R"({"url":"   ","title":"skipped","snippet":"x"},)"
        R"({"url":"https://c","title":"C","snippet":"sc"}]}})";
    // Capped at numResults; blank URLs skipped; array items that are not objects skipped.
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"A","url":"https://a","description":"sa"},)"
                         R"({"title":"B","url":"https://b","description":"sb"}]})"),
             websearch::normalize_kagi_search_response(body, "q", 2));
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"A","url":"https://a","description":"sa"},)"
                         R"({"title":"B","url":"https://b","description":"sb"},)"
                         R"({"title":"C","url":"https://c","description":"sc"}]})"),
             websearch::normalize_kagi_search_response(body, "q", 5));
    // Missing data / non-array search / everything skipped → no_results envelope.
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results"})"),
             websearch::normalize_kagi_search_response(R"({"data":{}})", "q", 5));
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results"})"),
             websearch::normalize_kagi_search_response(R"({"data":{"search":{}}})", "q", 5));
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results"})"),
             websearch::normalize_kagi_search_response(R"({"data":{"search":[{"url":""}]}})", "q", 5));
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results"})"),
             websearch::normalize_kagi_search_response(R"([1,2])", "q", 5));
    // Malformed JSON propagates like Kotlin's Json.parseToJsonElement (the caller maps it
    // to search_error).
    bool threw = false;
    try {
        websearch::normalize_kagi_search_response("not json", "q", 5);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(web_search_html_to_readable_text) {
    // Comments first, then script/style/head + nav/header/footer/aside via the tag backref,
    // case-insensitively; the decode seam sees the stripped markup; whitespace collapses.
    const std::string html =
        "<html><head><TITLE>T</TITLE></head><body><!-- note -->"
        "<SCRIPT>var x = 1;</SCRIPT><NAV>MENU</NAV><p>Hello&nbsp;&amp;&nbsp;World</p>"
        "<footer>Footer</footer></body>";
    std::string seen_by_decoder;
    const auto decoder = [&](const std::string& stripped) {
        seen_by_decoder = stripped;
        // HtmlCompat stand-in: tags removed, the two entities the input uses decoded.
        std::string out;
        bool in_tag = false;
        for (std::size_t i = 0; i < stripped.size(); ++i) {
            const char c = stripped[i];
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) out += c;
        }
        std::string decoded;
        std::size_t at = 0;
        while (true) {
            const std::size_t amp = out.find('&', at);
            if (amp == std::string::npos) {
                decoded.append(out, at, out.size() - at);
                break;
            }
            decoded.append(out, at, amp - at);
            if (out.compare(amp, 5, "&amp;") == 0) {
                decoded += '&';
                at = amp + 5;
            } else if (out.compare(amp, 6, "&nbsp;") == 0) {
                decoded += ' ';
                at = amp + 6;
            } else {
                decoded += '&';
                at = amp + 1;
            }
        }
        return decoded;
    };

    const std::string text = websearch::html_to_readable_text(html, decoder);
    // The decoder received comment/script/head/nav/footer stripped markup.
    CHECK(seen_by_decoder.find("<!--") == std::string::npos);
    CHECK(seen_by_decoder.find("var x") == std::string::npos);
    CHECK(seen_by_decoder.find("<TITLE>") == std::string::npos);
    CHECK(seen_by_decoder.find("MENU") == std::string::npos);
    CHECK(seen_by_decoder.find("Footer") == std::string::npos);
    // Content survives with the whitespace contract: single spaces, no blank runs, trimmed.
    CHECK_EQ(std::string("Hello & World"), text);

    // The 600 KB cap applies before the strip passes (String.take, UTF-16 units).
    const std::string huge(700'000, 'a');
    const auto identity = [](const std::string& value) { return value; };
    CHECK_EQ(static_cast<std::size_t>(600'000), websearch::html_to_readable_text(huge, identity).size());

    // Collapse rules: intra-line runs, spaces around newlines, 3+ blank lines → one blank line.
    const auto same = [](const std::string& value) { return value; };
    CHECK_EQ(std::string("a b\nc"),
             websearch::html_to_readable_text("   a   b \n  c  ", same));
    CHECK_EQ(std::string("a\n\nb"),
             websearch::html_to_readable_text("a\n\n\n\nb", same));
}

// ---- Definitions ---------------------------------------------------------------------------------

struct DefinitionsProvider {
    ScriptedTransport transport;
    WebSearchToolProvider provider;

    DefinitionsProvider() : provider(transport, tag_stripping_decoder()) {}
};

TEST(web_search_definitions_disabled_returns_empty) {
    DefinitionsProvider fixture;
    auto ctx = make_ctx();
    ctx.web_search_enabled = false;
    CHECK(fixture.provider.definitions(ctx).empty());
    // handles() is independent of the gate.
    CHECK(fixture.provider.handles("web_search"));
    CHECK(fixture.provider.handles("web_fetch"));
    CHECK(!fixture.provider.handles("execute_shell_command"));
}

TEST(web_search_definitions_exact_shapes) {
    DefinitionsProvider fixture;
    const auto definitions = fixture.provider.definitions(make_ctx());
    CHECK_EQ(std::size_t(2), definitions.size());

    CHECK_EQ(std::string("function"), definitions[0].type);
    CHECK_EQ(std::string("web_search"), definitions[0].function.name);
    CHECK_EQ(
        std::string("Search the web for current information. Use this to find facts, news, or "
                    "data not in your training set."),
        definitions[0].function.description);
    CHECK_EQ(std::string("object"), definitions[0].function.parameters.type);
    CHECK_EQ(std::size_t(2), definitions[0].function.parameters.properties.size());
    CHECK_EQ(std::string("query"), definitions[0].function.parameters.properties[0].first);
    CHECK_EQ(std::string("string"),
             definitions[0].function.parameters.properties[0].second.type);
    CHECK_EQ(std::string("The search query to execute."),
             definitions[0].function.parameters.properties[0].second.description);
    CHECK_EQ(std::string("num_results"), definitions[0].function.parameters.properties[1].first);
    CHECK_EQ(std::string("integer"),
             definitions[0].function.parameters.properties[1].second.type);
    CHECK_EQ(std::string("Number of results to return (1-10, default 5)."),
             definitions[0].function.parameters.properties[1].second.description);
    CHECK_EQ(std::size_t(1), definitions[0].function.parameters.required.size());
    CHECK_EQ(std::string("query"), definitions[0].function.parameters.required[0]);

    CHECK_EQ(std::string("web_fetch"), definitions[1].function.name);
    CHECK_EQ(
        std::string("Fetch and read the full text content of a web page. Use this after "
                    "web_search when you need more detail from a specific page."),
        definitions[1].function.description);
    CHECK_EQ(std::size_t(2), definitions[1].function.parameters.properties.size());
    CHECK_EQ(std::string("url"), definitions[1].function.parameters.properties[0].first);
    CHECK_EQ(std::string("The URL of the page to fetch."),
             definitions[1].function.parameters.properties[0].second.description);
    CHECK_EQ(std::string("maxChars"), definitions[1].function.parameters.properties[1].first);
    CHECK_EQ(std::string("integer"),
             definitions[1].function.parameters.properties[1].second.type);
    CHECK_EQ(
        std::string("Maximum characters of text to return (default 8000, max 100000). If the "
                    "result has \"truncated\": true, call again with a larger maxChars to get "
                    "more."),
        definitions[1].function.parameters.properties[1].second.description);
    CHECK_EQ(std::size_t(1), definitions[1].function.parameters.required.size());
    CHECK_EQ(std::string("url"), definitions[1].function.parameters.required[0]);
}

// ---- execute: dispatch and argument coercion ------------------------------------------------------

TEST(web_search_execute_unknown_tool) {
    DefinitionsProvider fixture;
    CHECK_EQ(std::string("Unknown tool: nope"),
             fixture.provider.execute("nope", "{}", make_ctx()));
}

TEST(web_search_execute_argument_coercion) {
    DefinitionsProvider fixture;
    ScriptedTransport& transport = fixture.transport;
    auto ctx = make_ctx();
    ctx.web_search_provider = "duckduckgo";
    transport.ddg_outcome = ddg_success({{"T", "u", "s"}});

    // Blank arguments behave as "{}" → no_query (no query member at all).
    CHECK_EQ(std::string(R"({"type":"web_search","error":"no_query"})"),
             fixture.provider.execute("web_search", "", ctx));
    CHECK_EQ(std::string(R"({"type":"web_search","error":"no_query"})"),
             fixture.provider.execute("web_search", R"({"num_results":3})", ctx));
    // A non-primitive query (object/array) fails the `as? JsonPrimitive` cast → no_query.
    CHECK_EQ(std::string(R"({"type":"web_search","error":"no_query"})"),
             fixture.provider.execute("web_search", R"({"query":{"q":"x"}})", ctx));

    // JsonNull IS a JsonPrimitive: content "null" reaches the search (kotlinx quirk, pinned).
    fixture.provider.execute("web_search", R"({"query":null})", ctx);
    CHECK_EQ(std::string("ddg"), transport.last().kind);
    CHECK_EQ(std::string("null"), transport.last().ddg_query);

    // num_results: primitive string/number parse, Int range, clamp 1..10, ctx fallback.
    fixture.provider.execute("web_search", R"({"query":"q","num_results":"3"})", ctx);
    CHECK_EQ(3, transport.last().ddg_num);
    fixture.provider.execute("web_search", R"({"query":"q","num_results":7})", ctx);
    CHECK_EQ(7, transport.last().ddg_num);
    fixture.provider.execute("web_search", R"({"query":"q","num_results":99})", ctx);
    CHECK_EQ(10, transport.last().ddg_num);
    fixture.provider.execute("web_search", R"({"query":"q","num_results":0})", ctx);
    CHECK_EQ(1, transport.last().ddg_num);
    fixture.provider.execute("web_search", R"({"query":"q","num_results":"abc"})", ctx);
    CHECK_EQ(5, transport.last().ddg_num);  // toIntOrNull null → ctx default
    fixture.provider.execute("web_search", R"({"query":"q","num_results":"5.0"})", ctx);
    CHECK_EQ(5, transport.last().ddg_num);
    fixture.provider.execute("web_search", R"({"query":"q"})", ctx);
    CHECK_EQ(5, transport.last().ddg_num);
    ctx.web_search_num_results = 4;
    fixture.provider.execute("web_search", R"({"query":"q","num_results":null})", ctx);
    CHECK_EQ(4, transport.last().ddg_num);  // JsonNull content "null" → toIntOrNull null → ctx
    // Overflow beyond Int → null → ctx default.
    fixture.provider.execute("web_search", R"({"query":"q","num_results":"9999999999"})", ctx);
    CHECK_EQ(4, transport.last().ddg_num);

    // Malformed argument JSON propagates before the try (kotlinx decode throws).
    bool threw = false;
    try {
        fixture.provider.execute("web_search", "{oops", ctx);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- execute: DuckDuckGo branch --------------------------------------------------------------------

TEST(web_search_execute_duckduckgo_branch) {
    DefinitionsProvider fixture;
    auto ctx = make_ctx();
    ctx.web_search_provider = "duckduckgo";

    fixture.transport.ddg_outcome =
        ddg_success({{"Title one", "https://one", "Snippet one"}, {"T2", "https://two", "S2"}});
    CHECK_EQ(std::string(R"({"type":"web_search","query":"weather","results":[)"
                         R"({"title":"Title one","url":"https://one","description":"Snippet one"},)"
                         R"({"title":"T2","url":"https://two","description":"S2"}]})"),
             fixture.provider.execute("web_search", R"({"query":"weather"})", ctx));

    fixture.transport.ddg_outcome = ddg_error(duck::SearchErrorType::NoResults, "nothing here");
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results",)"
                         R"("message":"nothing here"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    fixture.transport.ddg_outcome = ddg_error(duck::SearchErrorType::Captcha, "blocked");
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"captcha","message":"blocked"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    fixture.transport.ddg_outcome =
        ddg_error(duck::SearchErrorType::NetworkError, "Network error: down");
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"network_error",)"
                         R"("message":"Network error: down"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));
}

// ---- execute: API-key gate and request shapes ----------------------------------------------------

TEST(web_search_execute_api_key_gate) {
    DefinitionsProvider fixture;
    auto ctx = make_ctx();
    ctx.web_search_provider = "serper";

    // Missing key → no_api_key with the display name (before any network call).
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_api_key",)"
                         R"("provider":"Serper"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));
    CHECK(fixture.transport.calls.empty());

    // Blank key behaves as missing.
    ctx.web_search_api_keys["serper"] = "   ";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_api_key",)"
                         R"("provider":"Serper"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));
    CHECK(fixture.transport.calls.empty());

    // searxng skips the key gate entirely.
    ctx.web_search_provider = "searxng";
    fixture.transport.get_result = R"({"results":[]})";
    const std::string searxng = fixture.provider.execute("web_search", R"({"query":"q"})", ctx);
    CHECK_EQ(std::size_t(1), fixture.transport.calls.size());
    CHECK_EQ(std::string("https://searx.be/search?q=q&format=json"),
             fixture.transport.last().url);
    CHECK_EQ(std::string("get"), fixture.transport.last().kind);
}

TEST(web_search_execute_request_shapes) {
    DefinitionsProvider fixture;
    auto ctx = make_ctx();

    // kagi: POST body + Bearer header, then the Kagi normalizer runs on the response.
    ctx.web_search_provider = "kagi";
    ctx.web_search_api_keys["kagi"] = "k-key";
    fixture.transport.post_result =
        R"({"data":{"search":[{"url":"https://a","title":"A","snippet":"s"}]}})";
    const std::string kagi = fixture.provider.execute(
        "web_search", R"({"query":"cats","num_results":2})", ctx);
    CHECK_EQ(std::string("https://kagi.com/api/v1/search"), fixture.transport.last().url);
    CHECK_EQ(std::string(R"({"query":"cats","workflow":"search","limit":2})"),
             fixture.transport.last().body);
    CHECK_EQ(std::string("Bearer k-key"),
             fixture.transport.header_value("Authorization"));
    CHECK_EQ(std::string("application/json"), fixture.transport.header_value("Accept"));
    CHECK_EQ(60'000, fixture.transport.last().timeout_ms);
    CHECK_EQ(std::string(R"({"type":"web_search","query":"cats","results":[)"
                         R"({"title":"A","url":"https://a","description":"s"}]})"),
             kagi);

    // serper: {"q","num"} body + X-API-KEY.
    ctx.web_search_provider = "serper";
    ctx.web_search_api_keys["serper"] = "s-key";
    fixture.transport.post_result = R"({"organic":[{"title":"T","link":"https://l","snippet":"S"}]})";
    fixture.provider.execute("web_search", R"({"query":"news","num_results":3})", ctx);
    CHECK_EQ(std::string("https://google.serper.dev/search"), fixture.transport.last().url);
    CHECK_EQ(std::string(R"({"q":"news","num":3})"), fixture.transport.last().body);
    CHECK_EQ(std::string("s-key"), fixture.transport.header_value("X-API-KEY"));

    // tavily: fixed request body field order + no headers.
    ctx.web_search_provider = "tavily";
    ctx.web_search_api_keys["tavily"] = "t-key";
    fixture.transport.post_result = R"({"results":[]})";
    fixture.provider.execute("web_search", R"({"query":"deep"})", ctx);
    CHECK_EQ(std::string("https://api.tavily.com/search"), fixture.transport.last().url);
    CHECK_EQ(std::string(R"({"api_key":"t-key","query":"deep","max_results":5,)"
                         R"("search_depth":"advanced","include_answer":true})"),
             fixture.transport.last().body);
    CHECK(fixture.transport.last().headers.empty());

    // searxng: configured base URL + browser UA.
    ctx.web_search_provider = "searxng";
    ctx.web_search_base_url = "https://searx.local/";
    fixture.transport.get_result = R"({"results":[]})";
    fixture.provider.execute("web_search", R"({"query":"local"})", ctx);
    CHECK_EQ(std::string("https://searx.local/search?q=local&format=json"),
             fixture.transport.last().url);
    CHECK_EQ(std::string(websearch::kWebFetchUserAgent),
             fixture.transport.header_value("User-Agent"));

    // brave: q + count URL, subscription header.
    ctx.web_search_provider = "brave";
    ctx.web_search_api_keys["brave"] = "b-key";
    fixture.transport.get_result = R"({"organic":[]})";
    fixture.provider.execute("web_search", R"({"query":"two words","num_results":4})", ctx);
    CHECK_EQ(std::string("https://api.search.brave.com/res/v1/web/search?q=two+words&count=4"),
             fixture.transport.last().url);
    CHECK_EQ(std::string("b-key"), fixture.transport.header_value("X-Subscription-Token"));
    CHECK_EQ(std::string("application/json"), fixture.transport.header_value("Accept"));
}

// ---- execute: response normalizers ----------------------------------------------------------------

TEST(web_search_execute_tavily_normalizer) {
    DefinitionsProvider fixture;
    auto ctx = make_ctx();
    ctx.web_search_provider = "tavily";
    ctx.web_search_api_keys["tavily"] = "t-key";

    fixture.transport.post_result =
        std::string(
            R"({"answer":"The answer","results":[)"
            R"({"title":"T","url":"https://u","content":"c","score":"0.9506731"},)"
            R"({"title":"T2","url":"https://u2","content":"c2","score":"1.0"},)"
            R"({"title":"T3","url":"https://u3","content":"c3","score":"not-a-number"},)"
            R"({"title":"T4","url":"https://u4","content":"c4"})"
            R"(]})");
    CHECK_EQ(std::string(
                 R"({"type":"web_search","query":"q","answer":"The answer","results":[)"
                 R"({"title":"T","url":"https://u","content":"c","score":0.9506731},)"
                 R"({"title":"T2","url":"https://u2","content":"c2","score":1.0},)"
                 R"({"title":"T3","url":"https://u3","content":"c3"},)"
                 R"({"title":"T4","url":"https://u4","content":"c4"}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // A blank answer is omitted (isNullOrBlank); a missing results array or an empty one
    // degrades to no_results.
    fixture.transport.post_result =
        R"({"answer":"  ","results":[{"title":"T","url":"u","content":"c"}]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"T","url":"u","content":"c"}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));
    fixture.transport.post_result = R"({"answer":"a","results":[]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));
    fixture.transport.post_result = R"({"answer":"a"})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // The Java Float.toString layout for an integral score.
    fixture.transport.post_result =
        R"({"results":[{"title":"T","url":"u","content":"c","score":"2.0"}]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"T","url":"u","content":"c","score":2.0}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));
}

TEST(web_search_execute_generic_normalizer) {
    DefinitionsProvider fixture;
    auto ctx = make_ctx();
    ctx.web_search_provider = "brave";
    ctx.web_search_api_keys["brave"] = "b-key";

    // organic branch: title/link/snippet.
    fixture.transport.get_result =
        R"({"organic":[{"title":"T","link":"https://l","snippet":"S"}]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"T","url":"https://l","description":"S"}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // web.results branch: url/content.
    fixture.transport.get_result =
        R"({"web":{"results":[{"title":"T","url":"https://w","content":"C"}]}})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"T","url":"https://w","description":"C"}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // results branch: description fallbacks.
    fixture.transport.get_result =
        R"({"results":[{"title":"T","url":"https://r","description":"D"}]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"T","url":"https://r","description":"D"}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // A present-but-blank primitive `link` wins over `url` (Kotlin ?: stops at non-null).
    fixture.transport.get_result =
        R"({"results":[{"title":"T","link":"","url":"https://fallback","snippet":"S"}]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"T","url":"","description":"S"}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // Missing link falls through to url; snippet falls through content then description.
    fixture.transport.get_result =
        R"({"results":[{"title":"T","url":"https://u","content":"C","description":"D"}]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"T","url":"https://u","description":"C"}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));
    fixture.transport.get_result =
        R"({"results":[{"title":"T","url":"https://u","description":"D"}]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"T","url":"https://u","description":"D"}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // Non-primitive fields degrade to "".
    fixture.transport.get_result = R"({"results":[{"title":{"x":1},"url":["a"],"snippet":{}}]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","results":[)"
                         R"({"title":"","url":"","description":""}]})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // No known key, or an empty array → no_results.
    fixture.transport.get_result = R"({"something":"else"})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));
    fixture.transport.get_result = R"({"organic":[]})";
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_results"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // Wrong kind where the accessor demands a type → search_error (kotlinx throws).
    fixture.transport.get_result = R"({"organic":{"not":"an array"}})";
    std::string result = fixture.provider.execute("web_search", R"({"query":"q"})", ctx);
    CHECK(result.find(R"("error":"search_error")") != std::string::npos);
    CHECK(result.find("JsonArray") != std::string::npos);

    fixture.transport.get_result = R"({"web":"not-an-object"})";
    result = fixture.provider.execute("web_search", R"({"query":"q"})", ctx);
    CHECK(result.find(R"("error":"search_error")") != std::string::npos);
    CHECK(result.find("JsonObject") != std::string::npos);
}

TEST(web_search_execute_failure_envelopes) {
    DefinitionsProvider fixture;
    auto ctx = make_ctx();

    // Null response (Kotlin null: non-2xx / empty body) → no_response.
    ctx.web_search_provider = "brave";
    ctx.web_search_api_keys["brave"] = "b-key";
    fixture.transport.get_result = std::nullopt;
    CHECK_EQ(std::string(R"({"type":"web_search","query":"q","error":"no_response"})"),
             fixture.provider.execute("web_search", R"({"query":"q"})", ctx));

    // Transport exception (OkHttp IOException) → search_error with the message.
    ctx.web_search_provider = "searxng";
    fixture.transport.throw_on_call = true;
    fixture.transport.throw_message = "Connection reset";
    std::string result = fixture.provider.execute("web_search", R"({"query":"q"})", ctx);
    CHECK(result.find(R"("error":"search_error")") != std::string::npos);
    CHECK(result.find("Connection reset") != std::string::npos);

    // Malformed response body → search_error with the parse message.
    fixture.transport.throw_on_call = false;
    fixture.transport.get_result = "{oops";
    result = fixture.provider.execute("web_search", R"({"query":"q"})", ctx);
    CHECK(result.find(R"("error":"search_error")") != std::string::npos);
    CHECK(result.find("Unexpected JSON token") != std::string::npos);

    // A non-object response body → search_error.
    fixture.transport.get_result = "[1,2,3]";
    result = fixture.provider.execute("web_search", R"({"query":"q"})", ctx);
    CHECK(result.find(R"("error":"search_error")") != std::string::npos);
    CHECK(result.find("Expected JSON object") != std::string::npos);

    // The exception path keeps the captured query (it was read before the try).
    fixture.transport.throw_on_call = true;
    fixture.transport.throw_message = "io down";
    result = fixture.provider.execute("web_search", R"({"query":"kept"})", ctx);
    CHECK(result.find(R"("query":"kept")") != std::string::npos);
    CHECK(result.find(R"("error":"search_error")") != std::string::npos);
    CHECK(result.find("io down") != std::string::npos);
}

// ---- execute: web_fetch --------------------------------------------------------------------------

TEST(web_search_execute_web_fetch) {
    DefinitionsProvider fixture;
    auto ctx = make_ctx();

    // Missing url (absent or non-primitive) → no_url, no fetch call.
    CHECK_EQ(std::string(R"({"type":"web_fetch","error":"no_url"})"),
             fixture.provider.execute("web_fetch", "", ctx));
    CHECK_EQ(std::string(R"({"type":"web_fetch","error":"no_url"})"),
             fixture.provider.execute("web_fetch", R"({"url":{"nested":true}})", ctx));
    CHECK(fixture.transport.calls.empty());

    // Null response → no_response with the url.
    fixture.transport.get_result = std::nullopt;
    CHECK_EQ(std::string(R"({"type":"web_fetch","url":"https://x","error":"no_response"})"),
             fixture.provider.execute("web_fetch", R"({"url":"https://x"})", ctx));

    // Success: browser headers, decoded text, UTF-16-aware truncation.
    fixture.transport.get_result = "<p>Hello&nbsp;&amp;&nbsp;World</p>";
    const std::string ok =
        fixture.provider.execute("web_fetch", R"({"url":"https://x"})", ctx);
    CHECK_EQ(std::string(websearch::kWebFetchUserAgent),
             fixture.transport.header_value("User-Agent"));
    CHECK_EQ(std::string("text/html,application/xhtml+xml,*/*"),
             fixture.transport.header_value("Accept"));
    CHECK_EQ(60'000, fixture.transport.last().timeout_ms);
    // Default maxChars = 8000: the whole (short) text fits.
    CHECK_EQ(std::string(R"({"type":"web_fetch","url":"https://x","text":"Hello & World",)"
                         R"("truncated":false,"totalChars":13})"),
             ok);

    // maxChars coercion: explicit value, clamp high, clamp low, unparsable → default.
    fixture.transport.get_result = "aaaaaaaaaaaaaaaaaaaa";
    std::string fetched = fixture.provider.execute(
        "web_fetch", R"({"url":"https://x","maxChars":"5"})", ctx);
    CHECK_EQ(std::string(R"({"type":"web_fetch","url":"https://x","text":"aaaaa",)"
                         R"("truncated":true,"totalChars":20})"),
             fetched);
    fetched = fixture.provider.execute(
        "web_fetch", R"({"url":"https://x","maxChars":"500000"})", ctx);
    CHECK(fetched.find(R"("truncated":false)") != std::string::npos);
    CHECK(fetched.find(R"("totalChars":20)") != std::string::npos);
    fetched = fixture.provider.execute("web_fetch", R"({"url":"https://x","maxChars":0})", ctx);
    CHECK(fetched.find(R"("text":"a",)") != std::string::npos);  // clamp to 1
    fetched =
        fixture.provider.execute("web_fetch", R"({"url":"https://x","maxChars":"zz"})", ctx);
    CHECK(fetched.find(R"("truncated":false)") != std::string::npos);  // default 8000

    // UTF-16 unit accounting: an emoji is two units (Kotlin String semantics).
    fixture.transport.get_result = "ab\U0001F600cd";  // 6 UTF-16 units total
    fetched = fixture.provider.execute("web_fetch",
                                       R"({"url":"https://x","maxChars":4})", ctx);
    CHECK_EQ(std::string("{\"type\":\"web_fetch\",\"url\":\"https://x\",\"text\":"
                         "\"ab\U0001F600\",\"truncated\":true,\"totalChars\":6}"),
             fetched);

    // Transport exception → fetch_error with the message.
    fixture.transport.throw_on_call = true;
    fixture.transport.throw_message = "tls failed";
    const std::string failed = fixture.provider.execute("web_fetch", R"({"url":"https://x"})", ctx);
    CHECK(failed.find(R"("error":"fetch_error")") != std::string::npos);
    CHECK(failed.find("tls failed") != std::string::npos);
    CHECK(failed.find(R"("url":"https://x")") != std::string::npos);
}