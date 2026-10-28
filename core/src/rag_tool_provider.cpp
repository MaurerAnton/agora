#include "agora/core/rag_tool_provider.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "agora/core/json.hpp"
#include "agora/core/tz.hpp"

namespace agora::core::rag {

namespace {

namespace json = agora::core::json;

constexpr const char* kSearchConversations = "search_conversations";
constexpr const char* kListConversations = "list_conversations";
constexpr const char* kReadConversation = "read_conversation";

/** Constants.SEARCH_METHOD_RAG. */
constexpr const char* kSearchMethodRag = "rag";
/** ProviderDefaults.OPENAI_BASE_URL — the remote fallback when remoteBaseUrl is blank. */
constexpr const char* kOpenAiBaseUrl = "https://api.openai.com/v1";
/** Search step 5's total-message cap. */
constexpr int kTotalCap = 200;

// ---- JSON builders / Kotlin accessors ------------------------------------------------------------

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

json::Value json_array(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

std::string render(const json::Value& value) { return json::to_kotlin_string(value); }

const json::Value* map_get(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;  // last duplicate wins, like a Map
    }
    return found;
}

/** `(element as? JsonPrimitive)?.content`. */
std::optional<std::string> primitive_content(const json::Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == json::Kind::String || value->kind == json::Kind::Literal) return value->text;
    return std::nullopt;
}

/** Kotlin toIntOrNull: ASCII digits after an optional sign, Int range. */
std::optional<std::int32_t> kotlin_int(const std::string& text) {
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
        if (value > 2147483648LL) return std::nullopt;  // bounds the accumulation itself
    }
    if (negative) return static_cast<std::int32_t>(-value);
    if (value > 2147483647LL) return std::nullopt;
    return static_cast<std::int32_t>(value);
}

/** Kotlin String.lowercase() restricted to ASCII (the provider's tokens are ASCII). */
std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::int32_t clamp_i32(std::int32_t value, std::int32_t low, std::int32_t high) {
    return value < low ? low : (value > high ? high : value);
}

/** Kotlin Int addition for the `offset + limit < total` pagination checks (wraps, like JVM int). */
std::int32_t wrapped_add(std::int32_t left, std::int32_t right) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(left) +
                                     static_cast<std::uint32_t>(right));
}

/**
 * Java Float.toString — what kotlinx writes for a JsonPrimitive(Float): shortest round-trip
 * digits, plain decimal when the decimal exponent is in [-3, 6], computerized scientific
 * otherwise, always at least one fraction digit.
 */
std::string kotlin_float_string(float value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::scientific);
    if (result.ec != std::errc{}) throw std::runtime_error("float formatting failed");
    std::string scientific(buffer, result.ptr);
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
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

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

/**
 * Json.decodeFromString<Map<String, JsonElement>> outside every try block: blank arguments
 * become "{}", anything that is not an object throws out of execute() (like Kotlin).
 */
json::Value parse_arguments_object(const std::string& arguments) {
    const std::string text = json::is_blank(arguments) ? "{}" : arguments;
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        throw std::runtime_error("Expected a JSON object");
    }
    return *parsed.value;
}

/** SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US) over the provider's configured zone. */
using TimestampFn = std::function<std::string(std::int64_t)>;

// ---- Private result shapes ------------------------------------------------------------------------

/** SearchWindowPlan: message ids + score, before the hydration step. */
struct WindowPlan {
    std::string conversation_id;
    std::string conversation_title;
    std::vector<std::string> message_ids;
    float top_score = 0.0f;
    int match_count = 0;
};

/** One inclusive index range plus its score (Kotlin Pair<IntRange, Float>). */
struct WindowRange {
    int first = 0;
    int last = 0;
};

json::Value message_json(const SearchMessage& message, const TimestampFn& timestamp) {
    return json_object({{"participant",
                         json_string(RagToolProvider::participant_name(message.participant))},
                        {"text", json_string(message.text)},
                        {"timestamp", json_string(timestamp(message.timestamp))}});
}

/** One hydrated result window: title, conversation_id, top_score, match_count, messages. */
json::Value window_json(const WindowPlan& window, const std::vector<SearchMessage>& messages,
                        const TimestampFn& timestamp) {
    std::vector<json::Value> hydrated;
    hydrated.reserve(messages.size());
    for (const SearchMessage& message : messages) {
        hydrated.push_back(message_json(message, timestamp));
    }
    return json_object({{"title", json_string(window.conversation_title)},
                        {"conversation_id", json_string(window.conversation_id)},
                        {"top_score", json_literal(kotlin_float_string(window.top_score))},
                        {"match_count", json_literal(std::to_string(window.match_count))},
                        {"messages", json_array(std::move(hydrated))}});
}

json::Value no_search_results(const std::string& query) {
    return json_object({{"type", json_string(kSearchConversations)},
                        {"query", json_string(query)},
                        {"error", json_string("no_results")}});
}

}  // namespace

// ---- Static helpers -------------------------------------------------------------------------------

bool RagToolProvider::is_synthetic_message_id(const std::string& message_id) {
    return message_id.rfind("tool_", 0) == 0 || message_id.rfind("result_", 0) == 0 ||
           message_id.rfind("compact_", 0) == 0;
}

std::string RagToolProvider::participant_name(run_recovery::Participant participant) {
    switch (participant) {
        case run_recovery::Participant::user: return "USER";
        case run_recovery::Participant::model: return "MODEL";
        case run_recovery::Participant::error: return "ERROR";
    }
    return "USER";
}

std::vector<provider_context::MessageContextTopology>
RagToolProvider::build_selected_topology_branch(
    const std::vector<provider_context::MessageContextTopology>& all_messages,
    const std::optional<std::string>& selected_branches_json) {
    // selections: Map<parentId?, childId>; malformed input (or non-string values) collapses
    // to an empty map, like the Kotlin try/catch around decodeFromString<Map<String, String>>.
    std::map<std::optional<std::string>, std::string> selections;
    if (selected_branches_json.has_value()) {
        std::map<std::string, std::string> raw;
        bool valid = true;
        const json::ParseResult parsed = json::parse(*selected_branches_json);
        if (parsed.value && parsed.value->kind == json::Kind::Object) {
            for (const json::Member& member : parsed.value->members) {
                if (member.value.kind != json::Kind::String) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                for (const json::Member& member : parsed.value->members) {
                    raw[member.key] = member.value.text;  // duplicate keys: last wins
                }
            }
        } else {
            valid = false;
        }
        if (valid) {
            for (const auto& [key, value] : raw) {
                // The literal key "null" addresses the root (Kotlin mapKeys to a null parent).
                selections[key == "null" ? std::nullopt : std::optional<std::string>(key)] = value;
            }
        }
    }

    // groupBy parentId; each sibling group sorted by (timestamp, id). String ids compare
    // lexicographically (the pinned corpus uses ASCII ids).
    std::map<std::optional<std::string>, std::vector<provider_context::MessageContextTopology>>
        by_parent;
    for (const auto& message : all_messages) {
        by_parent[message.parent_id].push_back(message);
    }
    for (auto& [parent, siblings] : by_parent) {
        (void)parent;
        std::stable_sort(siblings.begin(), siblings.end(),
                         [](const provider_context::MessageContextTopology& left,
                            const provider_context::MessageContextTopology& right) {
                             if (left.timestamp != right.timestamp) {
                                 return left.timestamp < right.timestamp;
                             }
                             return left.id < right.id;
                         });
    }

    // Walk root → leaf, one child per level.
    std::vector<provider_context::MessageContextTopology> path;
    std::optional<std::string> parent_id;  // Kotlin starts at null (the root)
    while (true) {
        const auto found = by_parent.find(parent_id);
        if (found == by_parent.end()) break;
        const auto& siblings = found->second;
        if (siblings.empty()) break;

        const std::string* selected_id = nullptr;
        const auto selected = selections.find(parent_id);
        if (selected != selections.end()) selected_id = &selected->second;

        const provider_context::MessageContextTopology* chosen = nullptr;
        std::vector<const provider_context::MessageContextTopology*> visible;
        for (const auto& sibling : siblings) {
            if (!is_synthetic_message_id(sibling.id)) visible.push_back(&sibling);
        }
        if (!visible.empty()) {
            if (selected_id != nullptr) {
                for (const auto* candidate : visible) {
                    if (candidate->id == *selected_id) {
                        chosen = candidate;
                        break;
                    }
                }
            }
            if (chosen == nullptr) chosen = visible.back();
        } else {
            if (selected_id != nullptr) {
                for (const auto& candidate : siblings) {
                    if (candidate.id == *selected_id) {
                        chosen = &candidate;
                        break;
                    }
                }
            }
            if (chosen == nullptr) chosen = &siblings.back();
        }
        path.push_back(*chosen);
        parent_id = chosen->id;
    }
    return path;
}

std::string RagToolProvider::format_timestamp(std::int64_t utc_millis) const {
    const tz::CivilTime civil = tz::zone_for(timezone_id_).civil_at(utc_millis);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02d-%02d %02d:%02d:%02d",
                  static_cast<long long>(civil.year), civil.month, civil.day, civil.hour,
                  civil.minute, civil.second);
    return buffer;
}

// ---- Provider surface -----------------------------------------------------------------------------

RagToolProvider::RagToolProvider(ConversationSeam& conversations, EmbeddingSeam& embeddings,
                                 std::string timezone_id)
    : conversations_(conversations),
      embeddings_(embeddings),
      semantic_selector_(256),
      timezone_id_(std::move(timezone_id)) {}

bool RagToolProvider::handles(const std::string& name) const {
    return name == kSearchConversations || name == kListConversations ||
           name == kReadConversation;
}

std::vector<openai::ToolDefinition> RagToolProvider::definitions(
    const shell_provider::GenerationContext& ctx) const {
    if (!ctx.access_past_conversations) return {};
    std::vector<openai::ToolDefinition> definitions;
    {
        openai::ToolFunction function;
        function.name = kSearchConversations;
        function.description =
            "Search past conversations for relevant information. Use this to recall facts, "
            "decisions, or context from previous discussions.";
        openai::ToolProperty query;
        query.type = "string";
        query.description = "The search query to find relevant past conversations.";
        openai::ToolProperty limit;
        limit.type = "integer";
        limit.description = "Maximum number of results (1-20, default 10).";
        function.parameters.properties = {{"query", std::move(query)},
                                          {"limit", std::move(limit)}};
        function.parameters.required = {"query"};
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    {
        openai::ToolFunction function;
        function.name = kListConversations;
        function.description =
            "List all past conversations. Use this to browse conversation history and find "
            "conversations to read. Returns conversation IDs, titles, and last-updated "
            "timestamps.";
        openai::ToolProperty order;
        order.type = "string";
        order.description =
            "Sort order by last updated time: 'asc' (oldest first) or 'desc' (newest first). "
            "Default: 'desc'.";
        openai::ToolProperty limit;
        limit.type = "integer";
        limit.description = "Maximum conversations per page (1-50, default 20).";
        openai::ToolProperty offset;
        offset.type = "integer";
        offset.description = "Number of conversations to skip for pagination (default 0).";
        function.parameters.properties = {{"order", std::move(order)},
                                          {"limit", std::move(limit)},
                                          {"offset", std::move(offset)}};
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    {
        openai::ToolFunction function;
        function.name = kReadConversation;
        function.description =
            "Read a specific conversation by its ID. Shows the selected message branch as a "
            "linear list with page controls. Use this after list_conversations or "
            "search_conversations to read a conversation of interest. Each message includes "
            "participant, text, and timestamp.";
        openai::ToolProperty conversation_id;
        conversation_id.type = "string";
        conversation_id.description =
            "The conversation ID to read (from list_conversations or search_conversations "
            "results).";
        openai::ToolProperty offset;
        offset.type = "integer";
        offset.description = "Number of messages to skip for pagination (default 0).";
        openai::ToolProperty limit;
        limit.type = "integer";
        limit.description = "Maximum messages per page (1-100, default 50).";
        function.parameters.properties = {
            {"conversation_id", std::move(conversation_id)},
            {"offset", std::move(offset)},
            {"limit", std::move(limit)},
        };
        function.parameters.required = {"conversation_id"};
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    return definitions;
}

std::string RagToolProvider::execute(const std::string& name, const std::string& arguments,
                                     const shell_provider::GenerationContext& ctx) {
    if (name == kSearchConversations) return execute_search_conversations(arguments, ctx);
    if (name == kListConversations) return execute_list_conversations(arguments, ctx);
    if (name == kReadConversation) return execute_read_conversation(arguments, ctx);
    return "Unknown tool: " + name;
}

// ---- semanticSearch -------------------------------------------------------------------------------

std::vector<std::pair<SearchMessage, float>> RagToolProvider::semantic_search(
    const std::string& query, std::int32_t limit, const shell_provider::GenerationContext& ctx) {
    if (!ctx.active_embedding_config.has_value()) return {};
    const shell_provider::EmbeddingConfig& config = *ctx.active_embedding_config;

    std::optional<std::vector<float>> query_embedding;
    if (config.local) {
        if (!embeddings_.is_model_ready(config.local_file_path)) return {};
        query_embedding = embeddings_.compute_local_embedding(query, config.local_file_path);
    } else {
        // resolveEmbeddingApiKey: blank key is no key.
        if (json::is_blank(ctx.embedding_api_key)) return {};
        const std::string base_url =
            config.remote_base_url.empty() ? kOpenAiBaseUrl : config.remote_base_url;
        query_embedding = embeddings_.compute_remote_embedding(query, ctx.embedding_api_key,
                                                               config.remote_model_name, base_url);
    }
    if (!query_embedding.has_value()) return {};

    const embedding::Selection selection = semantic_selector_.select(
        *query_embedding, ctx.rag_threshold, limit,
        [this, &config](std::int64_t after_id, int page_limit) {
            return conversations_.get_embedding_search_page(
                config.id, after_id, kMinSemanticSearchTextLength, page_limit);
        });
    if (selection.candidates.empty()) return {};

    const std::vector<SearchMessage> rows =
        conversations_.get_searchable_messages_by_ids([&selection] {
            std::vector<std::string> ids;
            ids.reserve(selection.candidates.size());
            for (const embedding::Candidate& candidate : selection.candidates) {
                ids.push_back(candidate.message_id);
            }
            return ids;
        }());
    std::map<std::string, SearchMessage> messages_by_id;
    for (const auto& row : rows) messages_by_id[row.id] = row;

    std::vector<std::pair<SearchMessage, float>> results;
    for (const embedding::Candidate& candidate : selection.candidates) {
        const auto row = messages_by_id.find(candidate.message_id);
        if (row == messages_by_id.end()) continue;
        // Kotlin String.length counts UTF-16 code units.
        if (json::utf16_length(row->second.text) <
            static_cast<std::size_t>(kMinSemanticSearchTextLength)) {
            continue;
        }
        results.emplace_back(row->second, candidate.score);
    }
    return results;
}

// ---- search_conversations -------------------------------------------------------------------------

std::string RagToolProvider::execute_search_conversations(
    const std::string& arguments, const shell_provider::GenerationContext& ctx) {
    // Argument decoding sits outside the try, exactly like the Kotlin.
    const json::Value args = parse_arguments_object(arguments);

    const std::optional<std::string> query = primitive_content(map_get(args, "query"));
    if (!query.has_value()) {
        return render(json_object({{"type", json_string(kSearchConversations)},
                                   {"error", json_string("no_query")}}));
    }
    std::int32_t limit = ctx.search_match_limit;
    if (const auto raw = primitive_content(map_get(args, "limit"))) {
        if (const auto parsed = kotlin_int(*raw)) limit = *parsed;
    }
    limit = clamp_i32(limit, 1, 30);
    std::int32_t n = ctx.search_context_window;
    if (const auto raw = primitive_content(map_get(args, "context_window"))) {
        if (const auto parsed = kotlin_int(*raw)) n = *parsed;
    }
    n = clamp_i32(n, 4, 32);
    const int half_n = n / 2;
    const int max_window_size = n * 3;

    const TimestampFn stamp = [this](std::int64_t millis) { return format_timestamp(millis); };

    try {
        // Step 1: normalize to (message, score). The RAG path thresholds; the keyword path
        // does not.
        std::vector<std::pair<SearchMessage, float>> scored_results;
        if (ctx.model_search_method == kSearchMethodRag && ctx.active_embedding_config) {
            std::vector<std::pair<SearchMessage, float>> semantic =
                semantic_search(*query, limit, ctx);
            for (auto& entry : semantic) {
                if (entry.second >= ctx.rag_threshold) scored_results.push_back(std::move(entry));
            }
        } else {
            for (SearchMessage& message : conversations_.search_messages(*query, limit)) {
                scored_results.emplace_back(message, 1.0f);
            }
        }
        if (scored_results.empty()) return render(no_search_results(*query));

        // associate: last score wins per id.
        std::map<std::string, float> score_by_message_id;
        for (const auto& entry : scored_results) {
            score_by_message_id[entry.first.id] = entry.second;
        }
        // groupBy: first-seen conversation order, scored order within each conversation.
        std::vector<std::pair<std::string, std::vector<std::string>>> matches_by_conv;
        for (const auto& entry : scored_results) {
            auto group = std::find_if(matches_by_conv.begin(), matches_by_conv.end(),
                                      [&entry](const auto& candidate) {
                                          return candidate.first == entry.first.conversation_id;
                                      });
            if (group == matches_by_conv.end()) {
                matches_by_conv.emplace_back(entry.first.conversation_id,
                                             std::vector<std::string>{entry.first.id});
            } else {
                group->second.push_back(entry.first.id);
            }
        }
        if (matches_by_conv.empty()) return render(no_search_results(*query));

        // Steps 2-4: per conversation, rebuild the branch, expand and merge windows, cap.
        std::vector<WindowPlan> all_windows;
        for (const auto& [conv_id, match_ids] : matches_by_conv) {
            const std::optional<SearchableConversation> conversation =
                conversations_.get_searchable_conversation(conv_id);
            if (!conversation.has_value()) continue;

            std::vector<provider_context::MessageContextTopology> topology =
                conversations_.get_topology_snapshot(conv_id);
            topology.erase(std::remove_if(topology.begin(), topology.end(),
                                          [](const auto& row) {
                                              return row.participant ==
                                                     run_recovery::Participant::error;
                                          }),
                           topology.end());
            const std::vector<provider_context::MessageContextTopology> branch =
                build_selected_topology_branch(topology, conversation->selected_branches_json);

            const int branch_size = static_cast<int>(branch.size());
            // withIndex().associate: id → index (later duplicates overwrite).
            std::map<std::string, int> index_map;
            for (int i = 0; i < branch_size; ++i) index_map[branch[i].id] = i;

            // filter + toSet: unique ids in first-occurrence (scored) order.
            std::vector<std::string> branch_match_ids;
            for (const auto& id : match_ids) {
                if (index_map.count(id) == 0) continue;
                if (std::find(branch_match_ids.begin(), branch_match_ids.end(), id) !=
                    branch_match_ids.end()) {
                    continue;
                }
                branch_match_ids.push_back(id);
            }

            // Expand one window per match: N/2 on each side, asymmetric fill when a side
            // runs out of branch.
            std::vector<std::pair<WindowRange, float>> windows;
            for (const auto& match_id : match_ids) {
                const auto center_it = index_map.find(match_id);
                if (center_it == index_map.end()) continue;
                const int center_idx = center_it->second;
                float score = 1.0f;
                const auto score_it = score_by_message_id.find(match_id);
                if (score_it != score_by_message_id.end()) score = score_it->second;

                const int before = std::min(half_n, center_idx);
                const int after = std::min(half_n, branch_size - 1 - center_idx);
                const int extra_before =
                    std::min(half_n - before, branch_size - 1 - center_idx - after);
                const int extra_after =
                    std::max(0, std::min(half_n - after - extra_before, center_idx - before));
                const int start = std::max(0, center_idx - before - extra_after);
                const int end = std::min(branch_size - 1, center_idx + after + extra_before);
                windows.emplace_back(WindowRange{start, end}, score);
            }

            // Merge overlapping/adjacent windows: best score first (stable), and each window
            // merges into the FIRST existing entry it overlaps — Kotlin indexOfFirst.
            std::stable_sort(windows.begin(), windows.end(),
                             [](const auto& left, const auto& right) {
                                 return left.second > right.second;
                             });
            std::vector<std::pair<WindowRange, float>> merged;
            for (const auto& [range, score] : windows) {
                WindowRange merged_range = range;
                int overlap_idx = -1;
                for (int i = 0; i < static_cast<int>(merged.size()); ++i) {
                    const WindowRange& existing = merged[i].first;
                    if (merged_range.first <= existing.last + 1 &&
                        existing.first <= merged_range.last + 1) {
                        overlap_idx = i;
                        break;
                    }
                }
                if (overlap_idx >= 0) {
                    const float existing_score = merged[overlap_idx].second;
                    merged_range.first = std::min(merged_range.first, merged[overlap_idx].first.first);
                    merged_range.last = std::max(merged_range.last, merged[overlap_idx].first.last);
                    merged[overlap_idx] = {merged_range, std::max(score, existing_score)};
                } else {
                    merged.emplace_back(merged_range, score);
                }
            }

            // Cap oversized windows around the best-scoring match of this conversation.
            for (const auto& [range, score] : merged) {
                WindowRange capped = range;
                if (range.last - range.first + 1 > max_window_size) {
                    // Kotlin maxByOrNull: the FIRST maximum in branch-match order.
                    std::optional<std::string> center_id;
                    float best_score = 0.0f;
                    bool have_best = false;
                    for (const auto& id : branch_match_ids) {
                        float value = 0.0f;
                        const auto it = score_by_message_id.find(id);
                        if (it != score_by_message_id.end()) value = it->second;
                        if (!have_best || value > best_score) {
                            have_best = true;
                            best_score = value;
                            center_id = id;
                        }
                    }
                    const int center_idx =
                        center_id.has_value()
                            ? index_map[*center_id]
                            : (range.first + range.last) / 2;
                    capped.first = std::max(center_idx - half_n, range.first);
                    capped.last = std::min(center_idx + half_n, range.last);
                }

                const std::set<std::string> window_msg_ids(
                    [&branch, &capped] {
                        std::set<std::string> ids;
                        for (int i = capped.first; i <= capped.last; ++i) {
                            ids.insert(branch[i].id);
                        }
                        return ids;
                    }());
                const int matched_in_window = static_cast<int>(std::count_if(
                    branch_match_ids.begin(), branch_match_ids.end(),
                    [&window_msg_ids](const auto& id) { return window_msg_ids.count(id) > 0; }));

                WindowPlan plan;
                plan.conversation_id = conv_id;
                plan.conversation_title = conversation->title;
                for (int i = capped.first; i <= capped.last; ++i) {
                    if (!is_synthetic_message_id(branch[i].id)) {
                        plan.message_ids.push_back(branch[i].id);
                    }
                }
                plan.top_score = score;
                plan.match_count = matched_in_window;
                all_windows.push_back(std::move(plan));
            }
        }

        // Step 5: best-score windows first (stable), hydrate, cap the total message budget.
        std::stable_sort(all_windows.begin(), all_windows.end(),
                         [](const WindowPlan& left, const WindowPlan& right) {
                             return left.top_score > right.top_score;
                         });
        std::vector<std::pair<WindowPlan, std::vector<SearchMessage>>> final_windows;
        int total_messages = 0;
        for (const WindowPlan& window : all_windows) {
            if (total_messages >= kTotalCap) break;
            const int available = kTotalCap - total_messages;
            std::vector<std::string> selected_ids = window.message_ids;  // take(available)
            if (static_cast<int>(selected_ids.size()) > available) {
                selected_ids.resize(static_cast<std::size_t>(available));
            }
            const std::vector<SearchMessage> rows =
                conversations_.get_messages_by_ids(selected_ids);
            std::map<std::string, SearchMessage> messages_by_id;  // associateBy: last wins
            for (const auto& row : rows) messages_by_id[row.id] = row;
            std::vector<SearchMessage> hydrated;
            for (const auto& id : selected_ids) {
                const auto row = messages_by_id.find(id);
                if (row == messages_by_id.end()) continue;  // mapNotNull
                if (row->second.text.empty()) continue;     // filter text.isNotEmpty()
                hydrated.push_back(row->second);
            }
            final_windows.emplace_back(window, std::move(hydrated));
            total_messages += static_cast<int>(selected_ids.size());
        }

        // Step 6: format the output.
        std::vector<json::Value> results;
        results.reserve(final_windows.size());
        for (const auto& [window, hydrated] : final_windows) {
            results.push_back(window_json(window, hydrated, stamp));
        }
        return render(json_object({{"type", json_string(kSearchConversations)},
                                   {"query", json_string(*query)},
                                   {"count", json_literal(std::to_string(results.size()))},
                                   {"results", json_array(std::move(results))}}));
    } catch (const std::exception& error) {
        return render(json_object({{"type", json_string(kSearchConversations)},
                                   {"query", json_string(*query)},
                                   {"error", json_string("search_error")},
                                   {"message", json_string(error.what())}}));
    }
}

// ---- list_conversations ---------------------------------------------------------------------------

std::string RagToolProvider::execute_list_conversations(const std::string& arguments,
                                                        const shell_provider::GenerationContext&) {
    const json::Value args = parse_arguments_object(arguments);

    const std::string order =
        lower_ascii(primitive_content(map_get(args, "order")).value_or("desc"));
    std::int32_t limit = 20;
    if (const auto raw = primitive_content(map_get(args, "limit"))) {
        if (const auto parsed = kotlin_int(*raw)) limit = *parsed;
    }
    limit = clamp_i32(limit, 1, 50);
    std::int32_t offset = 0;
    if (const auto raw = primitive_content(map_get(args, "offset"))) {
        if (const auto parsed = kotlin_int(*raw)) offset = *parsed;
    }
    if (offset < 0) offset = 0;

    const TimestampFn stamp = [this](std::int64_t millis) { return format_timestamp(millis); };

    try {
        const std::int32_t total = conversations_.get_searchable_conversation_count();
        const std::vector<SearchableConversation> page = conversations_
            .get_searchable_conversations_page(offset, limit, order == "desc");
        const bool has_more = wrapped_add(offset, limit) < total;

        std::vector<json::Value> conversations;
        conversations.reserve(page.size());
        for (const SearchableConversation& conversation : page) {
            conversations.push_back(json_object(
                {{"id", json_string(conversation.id)},
                 {"title", json_string(conversation.title)},
                 {"timestamp", json_string(stamp(conversation.last_updated))}}));
        }
        return render(json_object({{"type", json_string(kListConversations)},
                                   {"total", json_literal(std::to_string(total))},
                                   {"offset", json_literal(std::to_string(offset))},
                                   {"limit", json_literal(std::to_string(limit))},
                                   {"has_more", json_literal(has_more ? "true" : "false")},
                                   {"conversations", json_array(std::move(conversations))}}));
    } catch (const std::exception& error) {
        return render(json_object({{"type", json_string(kListConversations)},
                                   {"error", json_string("list_error")},
                                   {"message", json_string(error.what())}}));
    }
}

// ---- read_conversation ----------------------------------------------------------------------------

std::string RagToolProvider::execute_read_conversation(
    const std::string& arguments, const shell_provider::GenerationContext&) {
    const json::Value args = parse_arguments_object(arguments);

    const std::string conversation_id =
        json::trim(primitive_content(map_get(args, "conversation_id")).value_or(""));
    if (conversation_id.empty()) {
        return render(json_object({{"type", json_string(kReadConversation)},
                                   {"error", json_string("missing_conversation_id")}}));
    }
    std::int32_t limit = 50;
    if (const auto raw = primitive_content(map_get(args, "limit"))) {
        if (const auto parsed = kotlin_int(*raw)) limit = *parsed;
    }
    limit = clamp_i32(limit, 1, 100);
    std::int32_t offset = 0;
    if (const auto raw = primitive_content(map_get(args, "offset"))) {
        if (const auto parsed = kotlin_int(*raw)) offset = *parsed;
    }
    if (offset < 0) offset = 0;

    const TimestampFn stamp = [this](std::int64_t millis) { return format_timestamp(millis); };

    try {
        const std::optional<SearchableConversation> conversation =
            conversations_.get_searchable_conversation(conversation_id);
        if (!conversation.has_value()) {
            return render(json_object({{"type", json_string(kReadConversation)},
                                       {"conversation_id", json_string(conversation_id)},
                                       {"error", json_string("not_found")}}));
        }

        std::vector<provider_context::MessageContextTopology> topology =
            conversations_.get_topology_snapshot(conversation_id);
        topology.erase(std::remove_if(topology.begin(), topology.end(),
                                      [](const auto& row) {
                                          return row.participant ==
                                                 run_recovery::Participant::error;
                                      }),
                       topology.end());
        std::vector<provider_context::MessageContextTopology> branch =
            build_selected_topology_branch(topology, conversation->selected_branches_json);
        branch.erase(std::remove_if(branch.begin(), branch.end(),
                                    [](const auto& row) {
                                        return is_synthetic_message_id(row.id);
                                    }),
                     branch.end());
        const int total_messages = static_cast<int>(branch.size());

        std::vector<provider_context::MessageContextTopology> page_topology;
        if (offset < total_messages) {
            // (offset + limit) wraps like Kotlin Int; a wrapped end below offset trips the
            // subList contract and lands in read_error, like ArrayList.subList.
            const int end = std::min(wrapped_add(offset, limit), total_messages);
            if (end < offset) {
                throw std::runtime_error("fromIndex(" + std::to_string(offset) +
                                         ") > toIndex(" + std::to_string(end) + ")");
            }
            page_topology.assign(branch.begin() + offset, branch.begin() + end);
        }
        std::vector<std::string> page_ids;
        page_ids.reserve(page_topology.size());
        for (const auto& row : page_topology) page_ids.push_back(row.id);

        const std::vector<SearchMessage> rows = conversations_.get_messages_by_ids(page_ids);
        std::map<std::string, SearchMessage> page_by_id;  // associateBy: last wins
        for (const auto& row : rows) page_by_id[row.id] = row;
        std::vector<SearchMessage> page;
        for (const auto& id : page_ids) {
            const auto row = page_by_id.find(id);
            if (row != page_by_id.end()) page.push_back(row->second);
        }
        const bool has_more = wrapped_add(offset, limit) < total_messages;

        std::vector<json::Value> messages;
        messages.reserve(page.size());
        for (const SearchMessage& message : page) {
            messages.push_back(message_json(message, stamp));
        }
        return render(json_object(
            {{"type", json_string(kReadConversation)},
             {"conversation_id", json_string(conversation_id)},
             {"title", json_string(conversation->title)},
             {"total_messages", json_literal(std::to_string(total_messages))},
             {"offset", json_literal(std::to_string(offset))},
             {"limit", json_literal(std::to_string(limit))},
             {"has_more", json_literal(has_more ? "true" : "false")},
             {"messages", json_array(std::move(messages))}}));
    } catch (const std::exception& error) {
        return render(json_object({{"type", json_string(kReadConversation)},
                                   {"conversation_id", json_string(conversation_id)},
                                   {"error", json_string("read_error")},
                                   {"message", json_string(error.what())}}));
    }
}

}  // namespace agora::core::rag
