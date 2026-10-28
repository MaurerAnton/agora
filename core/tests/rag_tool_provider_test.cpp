// Unit tests for the RagToolProvider port: the three gated definitions, the keyword and
// semantic search pipelines (window expansion/merge/cap, the 200-message budget, hydration
// filtering), the list/read pagination envelopes (including the JVM Int wrap in has_more),
// the branch builder, the seam failure envelopes and the timestamp zone handling.

#include "agora/core/rag_tool_provider.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"

namespace rag = agora::core::rag;
namespace shell_provider = agora::core::shell_provider;
namespace provider_context = agora::core::provider_context;
namespace run_recovery = agora::core::run_recovery;
namespace embedding = agora::core::embedding;

namespace {

using Participant = run_recovery::Participant;

rag::SearchMessage message(std::string id, std::string conversation_id, Participant participant,
                           std::int64_t timestamp, std::string text) {
    rag::SearchMessage row;
    row.id = std::move(id);
    row.conversation_id = std::move(conversation_id);
    row.participant = participant;
    row.timestamp = timestamp;
    row.text = std::move(text);
    return row;
}

provider_context::MessageContextTopology topology_row(std::string id,
                                                      std::optional<std::string> parent_id,
                                                      std::int64_t timestamp,
                                                      Participant participant) {
    provider_context::MessageContextTopology row;
    row.id = std::move(id);
    row.parent_id = std::move(parent_id);
    row.timestamp = timestamp;
    row.participant = participant;
    return row;
}

rag::SearchableConversation conversation_row(std::string id, std::string title,
                                             std::int64_t last_updated = 0,
                                             std::optional<std::string> selected_branches_json =
                                                 std::nullopt) {
    rag::SearchableConversation row;
    row.id = std::move(id);
    row.title = std::move(title);
    row.last_updated = last_updated;
    row.selected_branches_json = std::move(selected_branches_json);
    return row;
}

/** The app stores vectors big-endian, like ByteBuffer.order(BIG_ENDIAN). */
std::vector<std::uint8_t> encode_floats(const std::vector<float>& values) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(values.size() * 4);
    for (const float value : values) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 24));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 16));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 8));
        bytes.push_back(static_cast<std::uint8_t>(bits));
    }
    return bytes;
}

embedding::SearchRow embedding_row(std::int64_t id, std::string message_id,
                                   const std::vector<float>& values) {
    return embedding::SearchRow{id, std::move(message_id), encode_floats(values),
                                static_cast<int>(values.size())};
}

/** A linear branch: ids[0] is the root, every later id's parent is the previous id. */
std::vector<provider_context::MessageContextTopology> linear_topology(
    const std::vector<std::string>& ids, std::int64_t first_timestamp = 1000) {
    std::vector<provider_context::MessageContextTopology> rows;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        rows.push_back(topology_row(ids[i],
                                    i == 0 ? std::nullopt
                                           : std::optional<std::string>(ids[i - 1]),
                                    first_timestamp + static_cast<std::int64_t>(i),
                                    i % 2 == 0 ? Participant::user : Participant::model));
    }
    return rows;
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    std::size_t at = 0;
    while ((at = haystack.find(needle, at)) != std::string::npos) {
        ++count;
        at += needle.size();
    }
    return count;
}

struct FakeConversations final : rag::ConversationSeam {
    // Keyword search.
    std::vector<rag::SearchMessage> search_hits;
    int search_calls = 0;
    std::string last_query;
    std::int32_t last_limit = -1;

    // Conversation rows, topology and the hydration pool.
    std::map<std::string, rag::SearchableConversation> conversations;
    std::map<std::string, std::vector<provider_context::MessageContextTopology>> topology;
    std::map<std::string, rag::SearchMessage> pool;
    int conversation_calls = 0;
    std::string last_conversation_id;
    int topology_calls = 0;

    std::vector<std::vector<std::string>> messages_id_batches;
    std::vector<std::vector<std::string>> searchable_id_batches;

    // list_conversations.
    std::int32_t conversation_count = 0;
    std::vector<rag::SearchableConversation> conversation_page;
    int page_calls = 0;
    std::int32_t last_offset = -1;
    std::int32_t last_page_limit = -1;
    bool last_descending = true;

    // getEmbeddingSearchPage.
    std::vector<embedding::SearchRow> embedding_rows;
    int embedding_page_calls = 0;
    std::string last_model_id;
    std::int64_t last_after_id = -1;
    std::int32_t last_minimum_text_length = -1;

    std::optional<std::string> fail_with;

    void maybe_fail() const {
        if (fail_with.has_value()) throw std::runtime_error(*fail_with);
    }

    std::vector<rag::SearchMessage> search_messages(const std::string& query,
                                                    std::int32_t limit) override {
        maybe_fail();
        ++search_calls;
        last_query = query;
        last_limit = limit;
        std::vector<rag::SearchMessage> hits = search_hits;
        if (static_cast<std::int32_t>(hits.size()) > limit) {
            hits.resize(static_cast<std::size_t>(limit));
        }
        return hits;
    }

    std::optional<rag::SearchableConversation> get_searchable_conversation(
        const std::string& id) override {
        maybe_fail();
        ++conversation_calls;
        last_conversation_id = id;
        const auto found = conversations.find(id);
        if (found == conversations.end()) return std::nullopt;
        return found->second;
    }

    std::vector<provider_context::MessageContextTopology> get_topology_snapshot(
        const std::string& conversation_id) override {
        maybe_fail();
        ++topology_calls;
        const auto found = topology.find(conversation_id);
        if (found == topology.end()) return {};
        return found->second;
    }

    std::vector<rag::SearchMessage> hydrate(const std::vector<std::string>& ids) const {
        std::vector<rag::SearchMessage> rows;
        for (const std::string& id : ids) {
            const auto found = pool.find(id);
            if (found != pool.end()) rows.push_back(found->second);
        }
        return rows;
    }

    std::vector<rag::SearchMessage> get_messages_by_ids(
        const std::vector<std::string>& ids) override {
        maybe_fail();
        messages_id_batches.push_back(ids);
        return hydrate(ids);
    }

    std::vector<rag::SearchMessage> get_searchable_messages_by_ids(
        const std::vector<std::string>& ids) override {
        maybe_fail();
        searchable_id_batches.push_back(ids);
        return hydrate(ids);
    }

    std::int32_t get_searchable_conversation_count() override {
        maybe_fail();
        return conversation_count;
    }

    std::vector<rag::SearchableConversation> get_searchable_conversations_page(
        std::int32_t offset, std::int32_t limit, bool descending) override {
        maybe_fail();
        ++page_calls;
        last_offset = offset;
        last_page_limit = limit;
        last_descending = descending;
        return conversation_page;
    }

    std::vector<embedding::SearchRow> get_embedding_search_page(
        const std::string& model_id, std::int64_t after_id, std::int32_t minimum_text_length,
        std::int32_t limit) override {
        maybe_fail();
        ++embedding_page_calls;
        last_model_id = model_id;
        last_after_id = after_id;
        last_minimum_text_length = minimum_text_length;
        std::vector<embedding::SearchRow> page;
        for (const embedding::SearchRow& row : embedding_rows) {
            if (row.id > after_id) page.push_back(row);
        }
        if (static_cast<std::int32_t>(page.size()) > limit) {
            page.resize(static_cast<std::size_t>(limit));
        }
        return page;
    }
};

struct FakeEmbeddings final : rag::EmbeddingSeam {
    bool model_ready = true;
    std::optional<std::vector<float>> local_result = std::vector<float>{1.0f, 0.0f};
    std::optional<std::vector<float>> remote_result = std::vector<float>{1.0f, 0.0f};
    int ready_calls = 0;
    int local_calls = 0;
    int remote_calls = 0;
    std::string last_path;
    std::string last_query;
    std::string last_api_key;
    std::string last_model;
    std::string last_base_url;

    bool is_model_ready(const std::string& local_file_path) override {
        ++ready_calls;
        last_path = local_file_path;
        return model_ready;
    }

    std::optional<std::vector<float>> compute_local_embedding(
        const std::string& query, const std::string& local_file_path) override {
        ++local_calls;
        last_query = query;
        last_path = local_file_path;
        return local_result;
    }

    std::optional<std::vector<float>> compute_remote_embedding(
        const std::string& query, const std::string& api_key, const std::string& model,
        const std::string& base_url) override {
        ++remote_calls;
        last_query = query;
        last_api_key = api_key;
        last_model = model;
        last_base_url = base_url;
        return remote_result;
    }
};

struct Fixture {
    FakeConversations conversations;
    FakeEmbeddings embeddings;
    rag::RagToolProvider provider;

    Fixture() : provider(conversations, embeddings, "UTC") {}
};

shell_provider::GenerationContext make_ctx() {
    shell_provider::GenerationContext ctx;
    ctx.access_past_conversations = true;
    return ctx;
}

shell_provider::EmbeddingConfig local_config(std::string id = "emb1",
                                             std::string path = "/models/embed.gguf") {
    shell_provider::EmbeddingConfig config;
    config.id = std::move(id);
    config.local = true;
    config.local_file_path = std::move(path);
    return config;
}

shell_provider::EmbeddingConfig remote_config(std::string id = "emb1",
                                              std::string model = "text-embedding-3",
                                              std::string base_url = "") {
    shell_provider::EmbeddingConfig config;
    config.id = std::move(id);
    config.local = false;
    config.remote_model_name = std::move(model);
    config.remote_base_url = std::move(base_url);
    return config;
}

constexpr const char* kNoResults = R"({"type":"search_conversations","query":"q","error":"no_results"})";

/** Seed a conversation with a linear topology of ids and one pool row per id. */
void seed_linear(FakeConversations& seam, const std::string& conversation_id,
                 const std::string& title, const std::vector<std::string>& ids) {
    seam.conversations[conversation_id] = conversation_row(conversation_id, title);
    seam.topology[conversation_id] = linear_topology(ids);
    for (const std::string& id : ids) {
        seam.pool[id] = message(id, conversation_id, Participant::user, 0, "t-" + id);
    }
}

}  // namespace

// ---- Definitions -------------------------------------------------------------------------------

TEST(rag_definitions_honour_access_gate) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.access_past_conversations = false;
    CHECK(fixture.provider.definitions(ctx).empty());

    ctx.access_past_conversations = true;
    const auto definitions = fixture.provider.definitions(ctx);
    CHECK_EQ(std::size_t(3), definitions.size());
    CHECK_EQ(std::string("function"), definitions[0].type);
    CHECK_EQ(std::string("search_conversations"), definitions[0].function.name);
    CHECK_EQ(std::string("list_conversations"), definitions[1].function.name);
    CHECK_EQ(std::string("read_conversation"), definitions[2].function.name);

    CHECK_EQ(std::string("Search past conversations for relevant information. Use this to "
                         "recall facts, decisions, or context from previous discussions."),
             definitions[0].function.description);
    const auto& search_props = definitions[0].function.parameters.properties;
    CHECK_EQ(std::size_t(2), search_props.size());
    CHECK_EQ(std::string("query"), search_props[0].first);
    CHECK_EQ(std::string("string"), search_props[0].second.type);
    CHECK_EQ(std::string("The search query to find relevant past conversations."),
             search_props[0].second.description);
    CHECK_EQ(std::string("limit"), search_props[1].first);
    CHECK_EQ(std::string("integer"), search_props[1].second.type);
    CHECK_EQ(std::string("Maximum number of results (1-20, default 10)."),
             search_props[1].second.description);
    CHECK(definitions[0].function.parameters.required ==
          (std::vector<std::string>{"query"}));

    CHECK_EQ(std::string("List all past conversations. Use this to browse conversation "
                         "history and find conversations to read. Returns conversation IDs, "
                         "titles, and last-updated timestamps."),
             definitions[1].function.description);
    const auto& list_props = definitions[1].function.parameters.properties;
    CHECK_EQ(std::size_t(3), list_props.size());
    CHECK_EQ(std::string("order"), list_props[0].first);
    CHECK_EQ(std::string("Sort order by last updated time: 'asc' (oldest first) or 'desc' "
                         "(newest first). Default: 'desc'."),
             list_props[0].second.description);
    CHECK_EQ(std::string("limit"), list_props[1].first);
    CHECK_EQ(std::string("Maximum conversations per page (1-50, default 20)."),
             list_props[1].second.description);
    CHECK_EQ(std::string("offset"), list_props[2].first);
    CHECK_EQ(std::string("Number of conversations to skip for pagination (default 0)."),
             list_props[2].second.description);
    CHECK(definitions[1].function.parameters.required.empty());

    CHECK_EQ(std::string("Read a specific conversation by its ID. Shows the selected message "
                         "branch as a linear list with page controls. Use this after "
                         "list_conversations or search_conversations to read a conversation "
                         "of interest. Each message includes participant, text, and "
                         "timestamp."),
             definitions[2].function.description);
    const auto& read_props = definitions[2].function.parameters.properties;
    CHECK_EQ(std::size_t(3), read_props.size());
    CHECK_EQ(std::string("conversation_id"), read_props[0].first);
    CHECK_EQ(std::string("The conversation ID to read (from list_conversations or "
                         "search_conversations results)."),
             read_props[0].second.description);
    CHECK_EQ(std::string("offset"), read_props[1].first);
    CHECK_EQ(std::string("Number of messages to skip for pagination (default 0)."),
             read_props[1].second.description);
    CHECK_EQ(std::string("limit"), read_props[2].first);
    CHECK_EQ(std::string("Maximum messages per page (1-100, default 50)."),
             read_props[2].second.description);
    CHECK(definitions[2].function.parameters.required ==
          (std::vector<std::string>{"conversation_id"}));

    CHECK(fixture.provider.handles("search_conversations"));
    CHECK(fixture.provider.handles("list_conversations"));
    CHECK(fixture.provider.handles("read_conversation"));
    CHECK(!fixture.provider.handles("memory_read_file"));
}

TEST(rag_unknown_tool) {
    Fixture fixture;
    CHECK_EQ(std::string("Unknown tool: nope"),
             fixture.provider.execute("nope", "{}", make_ctx()));
}

// ---- search_conversations: argument handling ------------------------------------------------------

TEST(rag_search_requires_query) {
    Fixture fixture;
    const auto ctx = make_ctx();
    const std::string expected = R"({"type":"search_conversations","error":"no_query"})";
    CHECK_EQ(expected, fixture.provider.execute("search_conversations", R"({"limit":5})", ctx));
    CHECK_EQ(expected, fixture.provider.execute("search_conversations", "", ctx));
    CHECK_EQ(expected,
             fixture.provider.execute("search_conversations", R"({"query":{"nested":1}})", ctx));

    bool threw = false;
    try {
        fixture.provider.execute("search_conversations", R"({"query":)", ctx);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        fixture.provider.execute("search_conversations", "[1,2]", ctx);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(rag_search_limit_clamp) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.search_match_limit = 0;  // the context default is clamped too
    fixture.provider.execute("search_conversations", R"({"query":"q"})", ctx);
    CHECK_EQ(std::int32_t(1), fixture.conversations.last_limit);

    ctx.search_match_limit = 7;
    fixture.provider.execute("search_conversations", R"({"query":"q"})", ctx);
    CHECK_EQ(std::int32_t(7), fixture.conversations.last_limit);

    ctx = make_ctx();
    fixture.provider.execute("search_conversations", R"({"query":"q","limit":"99"})", ctx);
    CHECK_EQ(std::int32_t(30), fixture.conversations.last_limit);
    fixture.provider.execute("search_conversations", R"({"query":"q","limit":"0"})", ctx);
    CHECK_EQ(std::int32_t(1), fixture.conversations.last_limit);
    fixture.provider.execute("search_conversations", R"({"query":"q","limit":"abc"})", ctx);
    CHECK_EQ(std::int32_t(10), fixture.conversations.last_limit);
    fixture.provider.execute("search_conversations", R"({"query":"q","limit":5})", ctx);
    CHECK_EQ(std::int32_t(5), fixture.conversations.last_limit);
}

TEST(rag_search_context_window_clamp) {
    Fixture fixture;
    seed_linear(fixture.conversations, "c1", "T",
                {"m0", "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8", "m9"});
    fixture.conversations.search_hits = {message("m5", "c1", Participant::user, 0, "")};

    // "2" clamps up to 4 → halfN 2 → window [3..7].
    const std::string narrow = fixture.provider.execute(
        "search_conversations", R"({"query":"q","context_window":"2"})", make_ctx());
    CHECK_EQ(std::size_t(5), count_occurrences(narrow, "\"participant\""));
    CHECK(narrow.find("\"text\":\"t-m3\"") != std::string::npos);
    CHECK(narrow.find("\"text\":\"t-m7\"") != std::string::npos);
    CHECK(narrow.find("t-m2") == std::string::npos);

    // "99" clamps down to 32 → halfN 16 → the whole 10-message branch.
    const std::string wide = fixture.provider.execute(
        "search_conversations", R"({"query":"q","context_window":"99"})", make_ctx());
    CHECK_EQ(std::size_t(10), count_occurrences(wide, "\"participant\""));

    // A zero context default is clamped up to 4 as well.
    auto zero_ctx = make_ctx();
    zero_ctx.search_context_window = 0;
    const std::string clamped_default = fixture.provider.execute(
        "search_conversations", R"({"query":"q"})", zero_ctx);
    CHECK_EQ(std::size_t(5), count_occurrences(clamped_default, "\"participant\""));
}

// ---- search_conversations: envelopes and the window pipeline --------------------------------------

TEST(rag_search_keyword_envelope_exact) {
    Fixture fixture;
    seed_linear(fixture.conversations, "c1", "Alpha", {"u1", "m1", "u2"});
    fixture.conversations.pool["u1"] = message("u1", "c1", Participant::user, 0, "hello");
    fixture.conversations.pool["m1"] = message("m1", "c1", Participant::model, 0, "world");
    fixture.conversations.pool["u2"] = message("u2", "c1", Participant::user, 0, "third");
    fixture.conversations.search_hits = {message("m1", "c1", Participant::model, 0, "")};

    const std::string expected =
        R"({"type":"search_conversations","query":"find","count":1,"results":[)"
        R"({"title":"Alpha","conversation_id":"c1","top_score":1.0,"match_count":1,"messages":[)"
        R"({"participant":"USER","text":"hello","timestamp":"1970-01-01 00:00:00"},)"
        R"({"participant":"MODEL","text":"world","timestamp":"1970-01-01 00:00:00"},)"
        R"({"participant":"USER","text":"third","timestamp":"1970-01-01 00:00:00"}]}]})";
    CHECK_EQ(expected, fixture.provider.execute("search_conversations", R"({"query":"find"})",
                                                make_ctx()));
    CHECK_EQ(1, fixture.conversations.search_calls);
    CHECK_EQ(std::string("find"), fixture.conversations.last_query);
    CHECK_EQ(std::int32_t(10), fixture.conversations.last_limit);
    CHECK_EQ(std::size_t(1), fixture.conversations.messages_id_batches.size());
    CHECK(fixture.conversations.messages_id_batches[0] ==
          (std::vector<std::string>{"u1", "m1", "u2"}));
    CHECK(fixture.conversations.searchable_id_batches.empty());
}

TEST(rag_search_no_results_exact) {
    Fixture fixture;
    CHECK_EQ(std::string(kNoResults), fixture.provider.execute("search_conversations",
                                                                R"({"query":"q"})", make_ctx()));
}

TEST(rag_search_error_envelope) {
    Fixture fixture;
    fixture.conversations.fail_with = "boom";
    CHECK_EQ(std::string(R"({"type":"search_conversations","query":"q",)"
                         R"("error":"search_error","message":"boom"})"),
             fixture.provider.execute("search_conversations", R"({"query":"q"})", make_ctx()));
}

TEST(rag_search_merges_overlapping_windows) {
    Fixture fixture;
    seed_linear(fixture.conversations, "c1", "T",
                {"m0", "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8", "m9"});
    fixture.conversations.search_hits = {message("m0", "c1", Participant::user, 0, ""),
                                         message("m4", "c1", Participant::user, 0, "")};

    // n=4: [0..4] and [2..6] overlap → one window [0..6] with both matches.
    const std::string result = fixture.provider.execute(
        "search_conversations", R"({"query":"q","context_window":"4"})", make_ctx());
    CHECK(result.find("\"count\":1") != std::string::npos);
    CHECK_EQ(std::size_t(1), count_occurrences(result, "\"conversation_id\""));
    CHECK_EQ(std::size_t(7), count_occurrences(result, "\"participant\""));
    CHECK(result.find("\"match_count\":2") != std::string::npos);
}

TEST(rag_search_keeps_disjoint_windows) {
    Fixture fixture;
    std::vector<std::string> ids;
    for (int i = 0; i < 20; ++i) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "m%02d", i);
        ids.push_back(buffer);
    }
    seed_linear(fixture.conversations, "c1", "T", ids);
    fixture.conversations.search_hits = {message("m00", "c1", Participant::user, 0, ""),
                                         message("m19", "c1", Participant::user, 0, "")};

    // n=4: [0..4] and [15..19] are disjoint, so two windows survive.
    const std::string result = fixture.provider.execute(
        "search_conversations", R"({"query":"q","context_window":"4"})", make_ctx());
    CHECK(result.find("\"count\":2") != std::string::npos);
    CHECK_EQ(std::size_t(2), count_occurrences(result, "\"conversation_id\""));
    CHECK_EQ(std::size_t(10), count_occurrences(result, "\"participant\""));
    CHECK(result.find("\"match_count\":1") != std::string::npos);
    // Both scores are 1.0; the stable sort keeps the first window first.
    CHECK(result.find("\"text\":\"t-m00\"") < result.find("\"text\":\"t-m15\""));
}

TEST(rag_search_caps_oversized_window) {
    // n=4: the three windows [0..4], [4..8], [8..12] merge into [0..12] (13 > 12), so the
    // window is capped around the FIRST best-scoring match (all scores are 1.0).
    {
        Fixture fixture;
        std::vector<std::string> ids;
        for (int i = 0; i < 40; ++i) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "m%02d", i);
            ids.push_back(buffer);
        }
        seed_linear(fixture.conversations, "c1", "T", ids);
        fixture.conversations.search_hits = {message("m00", "c1", Participant::user, 0, ""),
                                             message("m06", "c1", Participant::user, 0, ""),
                                             message("m10", "c1", Participant::user, 0, "")};
        const std::string result = fixture.provider.execute(
            "search_conversations", R"({"query":"q","context_window":"4"})", make_ctx());
        CHECK(result.find("\"count\":1") != std::string::npos);
        CHECK_EQ(std::size_t(3), count_occurrences(result, "\"participant\""));
        CHECK(result.find("\"text\":\"t-m00\"") != std::string::npos);
        CHECK(result.find("\"text\":\"t-m02\"") != std::string::npos);
        CHECK(result.find("t-m03") == std::string::npos);
        CHECK(result.find("\"match_count\":1") != std::string::npos);
    }
    // Reordering the hits reorders branchMatchIds, and the cap follows the first maximum.
    {
        Fixture fixture;
        std::vector<std::string> ids;
        for (int i = 0; i < 40; ++i) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "m%02d", i);
            ids.push_back(buffer);
        }
        seed_linear(fixture.conversations, "c1", "T", ids);
        fixture.conversations.search_hits = {message("m06", "c1", Participant::user, 0, ""),
                                             message("m00", "c1", Participant::user, 0, ""),
                                             message("m10", "c1", Participant::user, 0, "")};
        const std::string result = fixture.provider.execute(
            "search_conversations", R"({"query":"q","context_window":"4"})", make_ctx());
        CHECK_EQ(std::size_t(5), count_occurrences(result, "\"participant\""));
        CHECK(result.find("\"text\":\"t-m04\"") != std::string::npos);
        CHECK(result.find("\"text\":\"t-m08\"") != std::string::npos);
        CHECK(result.find("t-m03") == std::string::npos);
    }
}

TEST(rag_search_caps_total_messages_at_200) {
    Fixture fixture;
    std::vector<std::string> ids;
    ids.reserve(400);
    for (int i = 0; i < 400; ++i) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "m%03d", i);
        ids.push_back(buffer);
    }
    seed_linear(fixture.conversations, "c1", "Big", ids);
    std::vector<rag::SearchMessage> hits;
    for (const char* id : {"m000", "m050", "m100", "m150", "m200", "m250", "m300"}) {
        hits.push_back(message(id, "c1", Participant::user, 0, ""));
    }
    fixture.conversations.search_hits = hits;

    // n=32 gives seven disjoint 33-message windows; 6 x 33 = 198 leaves room for 2 more.
    const std::string result = fixture.provider.execute(
        "search_conversations", R"({"query":"q","context_window":"32"})", make_ctx());
    CHECK(result.find("\"count\":7") != std::string::npos);
    CHECK_EQ(std::size_t(200), count_occurrences(result, "\"participant\""));
    CHECK(result.find("\"text\":\"t-m284\"") != std::string::npos);
    CHECK(result.find("\"text\":\"t-m286\"") == std::string::npos);
}

TEST(rag_search_hydration_filters_missing_and_empty) {
    Fixture fixture;
    fixture.conversations.conversations["c2"] = conversation_row("c2", "Hydra");
    fixture.conversations.topology["c2"] = linear_topology({"a", "b", "c", "d"});
    fixture.conversations.pool["a"] = message("a", "c2", Participant::user, 0, "keep-a");
    // "b" is missing from the pool entirely.
    fixture.conversations.pool["c"] = message("c", "c2", Participant::model, 0, "");
    fixture.conversations.pool["d"] = message("d", "c2", Participant::user, 0, "keep-d");
    fixture.conversations.search_hits = {message("b", "c2", Participant::user, 0, "")};

    const std::string result =
        fixture.provider.execute("search_conversations", R"({"query":"q"})", make_ctx());
    CHECK(result.find("\"count\":1") != std::string::npos);
    CHECK_EQ(std::size_t(2), count_occurrences(result, "\"participant\""));
    CHECK(result.find("keep-a") != std::string::npos);
    CHECK(result.find("keep-d") != std::string::npos);
    CHECK_EQ(std::size_t(1), fixture.conversations.messages_id_batches.size());
    CHECK_EQ(std::size_t(4), fixture.conversations.messages_id_batches[0].size());
}

TEST(rag_search_skips_conversations_without_row) {
    Fixture fixture;
    fixture.conversations.search_hits = {message("x", "ghost", Participant::user, 0, "")};
    CHECK_EQ(std::string(R"({"type":"search_conversations","query":"q","count":0,)"
                         R"("results":[]})"),
             fixture.provider.execute("search_conversations", R"({"query":"q"})", make_ctx()));
}

TEST(rag_search_filters_synthetic_ids_but_keeps_window) {
    Fixture fixture;
    fixture.conversations.conversations["c1"] = conversation_row("c1", "Syn");
    fixture.conversations.topology["c1"] = {
        topology_row("tool_x", std::nullopt, 1, Participant::model)};
    fixture.conversations.search_hits = {message("tool_x", "c1", Participant::model, 0, "")};

    CHECK_EQ(std::string(R"({"type":"search_conversations","query":"q","count":1,"results":[)"
                         R"({"title":"Syn","conversation_id":"c1","top_score":1.0,)"
                         R"("match_count":1,"messages":[]}]})"),
             fixture.provider.execute("search_conversations", R"({"query":"q"})", make_ctx()));
    CHECK_EQ(std::size_t(1), fixture.conversations.messages_id_batches.size());
    CHECK(fixture.conversations.messages_id_batches[0].empty());
}

// ---- search_conversations: semantic path ----------------------------------------------------------

TEST(rag_semantic_search_success) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.model_search_method = "rag";
    ctx.active_embedding_config = remote_config();
    ctx.embedding_api_key = "key-123";
    ctx.rag_threshold = 0.5f;

    fixture.conversations.conversations["sc"] = conversation_row("sc", "Semb");
    fixture.conversations.topology["sc"] = {
        topology_row("s1", std::nullopt, 10, Participant::user)};
    fixture.conversations.pool["s1"] = message("s1", "sc", Participant::user, 0,
                                               "semantic hit ok");
    fixture.conversations.embedding_rows = {embedding_row(1, "s1", {1.0f, 0.0f}),
                                            embedding_row(2, "s2", {0.0f, 1.0f})};
    fixture.embeddings.remote_result = std::vector<float>{1.0f, 0.0f};

    const std::string expected =
        R"({"type":"search_conversations","query":"find sem","count":1,"results":[)"
        R"({"title":"Semb","conversation_id":"sc","top_score":1.0,"match_count":1,"messages":[)"
        R"({"participant":"USER","text":"semantic hit ok","timestamp":"1970-01-01 00:00:00"}]}]})";
    CHECK_EQ(expected, fixture.provider.execute("search_conversations",
                                                R"({"query":"find sem"})", ctx));
    CHECK_EQ(0, fixture.conversations.search_calls);
    CHECK_EQ(1, fixture.embeddings.remote_calls);
    CHECK_EQ(std::string("find sem"), fixture.embeddings.last_query);
    CHECK_EQ(std::string("key-123"), fixture.embeddings.last_api_key);
    CHECK_EQ(std::string("text-embedding-3"), fixture.embeddings.last_model);
    CHECK_EQ(std::string("https://api.openai.com/v1"), fixture.embeddings.last_base_url);
    CHECK_EQ(1, fixture.conversations.embedding_page_calls);
    CHECK_EQ(std::string("emb1"), fixture.conversations.last_model_id);
    CHECK_EQ(std::int64_t(0), fixture.conversations.last_after_id);
    CHECK_EQ(std::int32_t(10), fixture.conversations.last_minimum_text_length);
    CHECK_EQ(std::size_t(1), fixture.conversations.searchable_id_batches.size());
    CHECK(fixture.conversations.searchable_id_batches[0] ==
          (std::vector<std::string>{"s1"}));
    CHECK_EQ(std::size_t(1), fixture.conversations.messages_id_batches.size());
}

TEST(rag_semantic_method_gates) {
    // The keyword method ignores an active embedding config.
    {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.active_embedding_config = remote_config();
        ctx.embedding_api_key = "k";
        CHECK_EQ(std::string(kNoResults), fixture.provider.execute(
                                              "search_conversations", R"({"query":"q"})", ctx));
        CHECK_EQ(1, fixture.conversations.search_calls);
        CHECK_EQ(0, fixture.embeddings.remote_calls);
        CHECK_EQ(0, fixture.conversations.embedding_page_calls);
    }
    // The RAG method without a config falls back to keyword search.
    {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.model_search_method = "rag";
        CHECK_EQ(std::string(kNoResults), fixture.provider.execute(
                                              "search_conversations", R"({"query":"q"})", ctx));
        CHECK_EQ(1, fixture.conversations.search_calls);
        CHECK_EQ(0, fixture.embeddings.remote_calls);
    }
}

TEST(rag_semantic_local_gate_and_failures) {
    // A local model that is not ready never computes an embedding.
    {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.model_search_method = "rag";
        ctx.active_embedding_config = local_config();
        fixture.embeddings.model_ready = false;
        CHECK_EQ(std::string(kNoResults), fixture.provider.execute(
                                              "search_conversations", R"({"query":"q"})", ctx));
        CHECK_EQ(1, fixture.embeddings.ready_calls);
        CHECK_EQ(std::string("/models/embed.gguf"), fixture.embeddings.last_path);
        CHECK_EQ(0, fixture.embeddings.local_calls);
    }
    // A failed local computation is an empty result list.
    {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.model_search_method = "rag";
        ctx.active_embedding_config = local_config();
        fixture.embeddings.local_result = std::nullopt;
        CHECK_EQ(std::string(kNoResults), fixture.provider.execute(
                                              "search_conversations", R"({"query":"q"})", ctx));
        CHECK_EQ(1, fixture.embeddings.ready_calls);
        CHECK_EQ(1, fixture.embeddings.local_calls);
        CHECK_EQ(std::string("q"), fixture.embeddings.last_query);
    }
}

TEST(rag_semantic_remote_key_gate) {
    // Blank and whitespace-only keys never reach the client (ifBlank → no key).
    for (const std::string& key : {std::string(""), std::string("   ")}) {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.model_search_method = "rag";
        ctx.active_embedding_config = remote_config();
        ctx.embedding_api_key = key;
        CHECK_EQ(std::string(kNoResults), fixture.provider.execute(
                                              "search_conversations", R"({"query":"q"})", ctx));
        CHECK_EQ(0, fixture.embeddings.remote_calls);
    }
    // A failed remote request is an empty result list.
    {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.model_search_method = "rag";
        ctx.active_embedding_config = remote_config();
        ctx.embedding_api_key = "k";
        fixture.embeddings.remote_result = std::nullopt;
        CHECK_EQ(std::string(kNoResults), fixture.provider.execute(
                                              "search_conversations", R"({"query":"q"})", ctx));
        CHECK_EQ(1, fixture.embeddings.remote_calls);
        CHECK_EQ(std::string("https://api.openai.com/v1"), fixture.embeddings.last_base_url);
    }
    // An explicit base URL is forwarded unchanged.
    {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.model_search_method = "rag";
        ctx.active_embedding_config = remote_config("emb1", "m", "https://custom.example/v1");
        ctx.embedding_api_key = "k";
        fixture.embeddings.remote_result = std::nullopt;
        fixture.provider.execute("search_conversations", R"({"query":"q"})", ctx);
        CHECK_EQ(std::string("https://custom.example/v1"), fixture.embeddings.last_base_url);
    }
}

TEST(rag_semantic_threshold_and_length_filters) {
    // A threshold above every score leaves no results.
    {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.model_search_method = "rag";
        ctx.active_embedding_config = remote_config();
        ctx.embedding_api_key = "k";
        ctx.rag_threshold = 1.5f;
        fixture.conversations.conversations["sc"] = conversation_row("sc", "S");
        fixture.conversations.topology["sc"] = {
            topology_row("s1", std::nullopt, 1, Participant::user)};
        fixture.conversations.pool["s1"] = message("s1", "sc", Participant::user, 0,
                                                   "long enough text");
        fixture.conversations.embedding_rows = {embedding_row(1, "s1", {1.0f, 0.0f})};
        CHECK_EQ(std::string(kNoResults), fixture.provider.execute(
                                              "search_conversations", R"({"query":"q"})", ctx));
    }
    // Kotlin String.length counts UTF-16 units: three astral characters plus two letters is
    // 8 units, below the 10-unit floor, even though the UTF-8 byte length is 14.
    {
        Fixture fixture;
        auto ctx = make_ctx();
        ctx.model_search_method = "rag";
        ctx.active_embedding_config = remote_config();
        ctx.embedding_api_key = "k";
        fixture.conversations.conversations["sc"] = conversation_row("sc", "S");
        fixture.conversations.topology["sc"] = {
            topology_row("s1", std::nullopt, 1, Participant::user)};
        fixture.conversations.pool["s1"] =
            message("s1", "sc", Participant::user, 0, "\xF0\x9D\x84\x9E\xF0\x9D\x84\x9E\xF0\x9D\x84\x9E"
                                                         "ab");
        fixture.conversations.embedding_rows = {embedding_row(1, "s1", {1.0f, 0.0f})};
        CHECK_EQ(std::string(kNoResults), fixture.provider.execute(
                                              "search_conversations", R"({"query":"q"})", ctx));

        // 8 + 8 = 16 units clears the floor.
        fixture.conversations.pool["s1"].text =
            "\xF0\x9D\x84\x9E\xF0\x9D\x84\x9E\xF0\x9D\x84\x9E"
            "abcdefgh";
        const std::string result =
            fixture.provider.execute("search_conversations", R"({"query":"q"})", ctx);
        CHECK_EQ(std::size_t(1), count_occurrences(result, "\"participant\""));
    }
}

TEST(rag_search_sorts_windows_by_score) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.model_search_method = "rag";
    ctx.active_embedding_config = remote_config();
    ctx.embedding_api_key = "k";
    ctx.rag_threshold = -1.0f;  // keep both scores, including 0.0
    fixture.embeddings.remote_result = std::vector<float>{1.0f, 0.0f};
    fixture.conversations.conversations["ca"] = conversation_row("ca", "First");
    fixture.conversations.conversations["cb"] = conversation_row("cb", "Second");
    fixture.conversations.topology["ca"] = {
        topology_row("sa", std::nullopt, 1, Participant::user)};
    fixture.conversations.topology["cb"] = {
        topology_row("sb", std::nullopt, 1, Participant::user)};
    fixture.conversations.pool["sa"] = message("sa", "ca", Participant::user, 0, "alpha text ok");
    fixture.conversations.pool["sb"] = message("sb", "cb", Participant::user, 0, "beta text ok!");
    fixture.conversations.embedding_rows = {embedding_row(1, "sa", {1.0f, 0.0f}),
                                            embedding_row(2, "sb", {0.0f, 1.0f})};

    const std::string result =
        fixture.provider.execute("search_conversations", R"({"query":"q"})", ctx);
    CHECK(result.find("\"count\":2") != std::string::npos);
    CHECK(result.find("\"conversation_id\":\"ca\"") <
          result.find("\"conversation_id\":\"cb\""));
    CHECK(result.find("\"top_score\":1.0") != std::string::npos);
    CHECK(result.find("\"top_score\":0.0") != std::string::npos);
}

// ---- list_conversations ---------------------------------------------------------------------------

TEST(rag_list_conversations_envelope_exact) {
    Fixture fixture;
    fixture.conversations.conversation_count = 50;
    fixture.conversations.conversation_page = {conversation_row("c2", "Beta", 0),
                                               conversation_row("c1", "Alpha", 946684800000LL)};

    const std::string expected =
        R"({"type":"list_conversations","total":50,"offset":0,"limit":20,"has_more":true,)"
        R"("conversations":[{"id":"c2","title":"Beta","timestamp":"1970-01-01 00:00:00"},)"
        R"({"id":"c1","title":"Alpha","timestamp":"2000-01-01 00:00:00"}]})";
    CHECK_EQ(expected, fixture.provider.execute("list_conversations", "", make_ctx()));
    CHECK_EQ(1, fixture.conversations.page_calls);
    CHECK_EQ(std::int32_t(0), fixture.conversations.last_offset);
    CHECK_EQ(std::int32_t(20), fixture.conversations.last_page_limit);
    CHECK(fixture.conversations.last_descending);
}

TEST(rag_list_conversations_args_and_has_more_wrap) {
    Fixture fixture;
    fixture.conversations.conversation_count = 5;

    fixture.provider.execute("list_conversations",
                             R"({"order":"ASC","limit":"99","offset":"-5"})", make_ctx());
    CHECK(!fixture.conversations.last_descending);
    CHECK_EQ(std::int32_t(50), fixture.conversations.last_page_limit);
    CHECK_EQ(std::int32_t(0), fixture.conversations.last_offset);

    fixture.provider.execute("list_conversations", R"({"limit":"0"})", make_ctx());
    CHECK_EQ(std::int32_t(1), fixture.conversations.last_page_limit);

    fixture.provider.execute("list_conversations", R"({"limit":"abc","offset":"7"})",
                             make_ctx());
    CHECK_EQ(std::int32_t(20), fixture.conversations.last_page_limit);
    CHECK_EQ(std::int32_t(7), fixture.conversations.last_offset);

    // offset + limit wraps like a JVM Int: 2147483647 + 20 is negative, so has_more is true.
    fixture.conversations.conversation_page.clear();
    CHECK_EQ(std::string(R"({"type":"list_conversations","total":5,"offset":2147483647,)"
                         R"("limit":20,"has_more":true,"conversations":[]})"),
             fixture.provider.execute("list_conversations", R"({"offset":"2147483647"})",
                                      make_ctx()));
}

TEST(rag_list_conversations_error_envelope) {
    Fixture fixture;
    fixture.conversations.fail_with = "disk on fire";
    CHECK_EQ(std::string(R"({"type":"list_conversations","error":"list_error",)"
                         R"("message":"disk on fire"})"),
             fixture.provider.execute("list_conversations", "{}", make_ctx()));
}

// ---- read_conversation ----------------------------------------------------------------------------

TEST(rag_read_conversation_envelope_exact) {
    Fixture fixture;
    fixture.conversations.conversations["c1"] = conversation_row("c1", "Alpha");
    fixture.conversations.topology["c1"] = {
        topology_row("u1", std::nullopt, 1000, Participant::user),
        topology_row("m1", "u1", 2000, Participant::model),
        topology_row("u2", "m1", 3000, Participant::user),
        topology_row("result_x", "u2", 4000, Participant::model),
        topology_row("e1", "u2", 3500, Participant::error),
    };
    fixture.conversations.pool["u1"] = message("u1", "c1", Participant::user, 0, "first");
    fixture.conversations.pool["m1"] = message("m1", "c1", Participant::model, 0, "middle");
    fixture.conversations.pool["u2"] = message("u2", "c1", Participant::user, 0, "last");

    const std::string expected =
        R"({"type":"read_conversation","conversation_id":"c1","title":"Alpha",)"
        R"("total_messages":3,"offset":1,"limit":1,"has_more":true,"messages":[)"
        R"({"participant":"MODEL","text":"middle","timestamp":"1970-01-01 00:00:00"}]})";
    CHECK_EQ(expected,
             fixture.provider.execute(
                 "read_conversation",
                 R"({"conversation_id":" c1 ","offset":"1","limit":"1"})", make_ctx()));
    CHECK_EQ(std::string("c1"), fixture.conversations.last_conversation_id);
    CHECK(fixture.conversations.searchable_id_batches.empty());
}

TEST(rag_read_conversation_missing_id) {
    Fixture fixture;
    const std::string expected =
        R"({"type":"read_conversation","error":"missing_conversation_id"})";
    CHECK_EQ(expected, fixture.provider.execute("read_conversation", "{}", make_ctx()));
    CHECK_EQ(expected, fixture.provider.execute("read_conversation",
                                                R"({"conversation_id":"   "})", make_ctx()));
}

TEST(rag_read_conversation_not_found) {
    Fixture fixture;
    CHECK_EQ(std::string(R"({"type":"read_conversation","conversation_id":"ghost",)"
                         R"("error":"not_found"})"),
             fixture.provider.execute("read_conversation", R"({"conversation_id":"ghost"})",
                                      make_ctx()));
}

TEST(rag_read_conversation_pagination_edges) {
    Fixture fixture;
    fixture.conversations.conversations["c1"] = conversation_row("c1", "Alpha");
    fixture.conversations.topology["c1"] = linear_topology({"a", "b", "c"});
    for (const char* id : {"a", "b", "c"}) {
        fixture.conversations.pool[id] = message(id, "c1", Participant::user, 0, "t");
    }

    // An offset past the branch yields an empty page and no more pages.
    CHECK_EQ(std::string(R"({"type":"read_conversation","conversation_id":"c1",)"
                         R"("title":"Alpha","total_messages":3,"offset":5,"limit":50,)"
                         R"("has_more":false,"messages":[]})"),
             fixture.provider.execute("read_conversation",
                                      R"({"conversation_id":"c1","offset":"5"})", make_ctx()));

    // Int overflow in offset + limit makes has_more true while the page stays empty.
    CHECK_EQ(std::string(R"({"type":"read_conversation","conversation_id":"c1",)"
                         R"("title":"Alpha","total_messages":3,"offset":2147483647,)"
                         R"("limit":50,"has_more":true,"messages":[]})"),
             fixture.provider.execute(
                 "read_conversation",
                 R"({"conversation_id":"c1","offset":"2147483647"})", make_ctx()));

    // limit clamps to 100, offset to 0.
    const std::string clamped = fixture.provider.execute(
        "read_conversation",
        R"({"conversation_id":"c1","limit":"999","offset":"-2"})", make_ctx());
    CHECK(clamped.find("\"offset\":0") != std::string::npos);
    CHECK(clamped.find("\"limit\":100") != std::string::npos);
    CHECK_EQ(std::size_t(3), count_occurrences(clamped, "\"participant\""));
}

TEST(rag_read_conversation_error_envelope) {
    Fixture fixture;
    fixture.conversations.fail_with = "kaput";
    CHECK_EQ(std::string(R"({"type":"read_conversation","conversation_id":"c1",)"
                         R"("error":"read_error","message":"kaput"})"),
             fixture.provider.execute("read_conversation", R"({"conversation_id":"c1"})",
                                      make_ctx()));
}

// ---- Branch builder --------------------------------------------------------------------------------

TEST(rag_branch_builder_selects_and_defaults) {
    const std::vector<provider_context::MessageContextTopology> messages = {
        topology_row("r_a", std::nullopt, 1, Participant::user),
        topology_row("r_b", std::nullopt, 2, Participant::user),
        topology_row("c_a1", "r_a", 3, Participant::model),
        topology_row("c_a2", "r_a", 4, Participant::model),
        topology_row("c_b1", "r_b", 3, Participant::model),
    };
    // Explicit root and child selections win.
    auto path = rag::RagToolProvider::build_selected_topology_branch(
        messages, std::optional<std::string>(R"({"null":"r_a","r_a":"c_a1"})"));
    CHECK_EQ(std::size_t(2), path.size());
    CHECK_EQ(std::string("r_a"), path[0].id);
    CHECK_EQ(std::string("c_a1"), path[1].id);

    // No selections: the last sibling (latest timestamp) is chosen at every level.
    path = rag::RagToolProvider::build_selected_topology_branch(messages, std::nullopt);
    CHECK_EQ(std::size_t(2), path.size());
    CHECK_EQ(std::string("r_b"), path[0].id);
    CHECK_EQ(std::string("c_b1"), path[1].id);
}

TEST(rag_branch_builder_tie_breaks_by_id) {
    // Equal timestamps sort by id, and the default pick is the last sibling.
    const std::vector<provider_context::MessageContextTopology> messages = {
        topology_row("r_y", std::nullopt, 5, Participant::user),
        topology_row("r_x", std::nullopt, 5, Participant::user),
    };
    const auto path = rag::RagToolProvider::build_selected_topology_branch(messages, std::nullopt);
    CHECK_EQ(std::size_t(1), path.size());
    CHECK_EQ(std::string("r_y"), path[0].id);
}

TEST(rag_branch_builder_synthetic_fallback) {
    const std::vector<provider_context::MessageContextTopology> all_synthetic = {
        topology_row("tool_a", std::nullopt, 1, Participant::model),
        topology_row("tool_b", std::nullopt, 2, Participant::model),
    };
    auto path = rag::RagToolProvider::build_selected_topology_branch(all_synthetic, std::nullopt);
    CHECK_EQ(std::size_t(1), path.size());
    CHECK_EQ(std::string("tool_b"), path[0].id);

    path = rag::RagToolProvider::build_selected_topology_branch(
        all_synthetic, std::optional<std::string>(R"({"null":"tool_a"})"));
    CHECK_EQ(std::string("tool_a"), path[0].id);

    // A selection pointing at a synthetic sibling is ignored while a visible one exists.
    const std::vector<provider_context::MessageContextTopology> mixed = {
        topology_row("tool_x", std::nullopt, 1, Participant::model),
        topology_row("real_r", std::nullopt, 2, Participant::user),
    };
    path = rag::RagToolProvider::build_selected_topology_branch(
        mixed, std::optional<std::string>(R"({"null":"tool_x"})"));
    CHECK_EQ(std::size_t(1), path.size());
    CHECK_EQ(std::string("real_r"), path[0].id);
}

TEST(rag_branch_builder_malformed_selections) {
    const std::vector<provider_context::MessageContextTopology> messages = {
        topology_row("r_a", std::nullopt, 1, Participant::user),
        topology_row("r_b", std::nullopt, 2, Participant::user),
    };
    auto path = rag::RagToolProvider::build_selected_topology_branch(
        messages, std::optional<std::string>("not-json"));
    CHECK_EQ(std::size_t(1), path.size());
    CHECK_EQ(std::string("r_b"), path[0].id);

    path = rag::RagToolProvider::build_selected_topology_branch(
        messages, std::optional<std::string>(R"({"null":42})"));
    CHECK_EQ(std::string("r_b"), path[0].id);

    path = rag::RagToolProvider::build_selected_topology_branch(
        messages, std::optional<std::string>("[]"));
    CHECK_EQ(std::string("r_b"), path[0].id);

    CHECK(rag::RagToolProvider::build_selected_topology_branch({}, std::nullopt).empty());
}

// ---- Small static helpers --------------------------------------------------------------------------

TEST(rag_synthetic_id_prefixes) {
    CHECK(rag::RagToolProvider::is_synthetic_message_id("tool_1"));
    CHECK(rag::RagToolProvider::is_synthetic_message_id("result_1"));
    CHECK(rag::RagToolProvider::is_synthetic_message_id("compact_1"));
    CHECK(!rag::RagToolProvider::is_synthetic_message_id("tools_1"));
    CHECK(!rag::RagToolProvider::is_synthetic_message_id("Tool_1"));
    CHECK(!rag::RagToolProvider::is_synthetic_message_id("abc"));
}

TEST(rag_participant_names) {
    CHECK_EQ(std::string("USER"), rag::RagToolProvider::participant_name(Participant::user));
    CHECK_EQ(std::string("MODEL"), rag::RagToolProvider::participant_name(Participant::model));
    CHECK_EQ(std::string("ERROR"), rag::RagToolProvider::participant_name(Participant::error));
}

// ---- Timestamps ------------------------------------------------------------------------------------

TEST(rag_timestamps_follow_configured_zone) {
    // UTC (the fixture default) formats the epoch directly, including pre-epoch instants.
    {
        Fixture fixture;
        fixture.conversations.conversation_count = 1;
        fixture.conversations.conversation_page = {conversation_row("c", "T", -1)};
        const std::string result =
            fixture.provider.execute("list_conversations", "{}", make_ctx());
        CHECK(result.find("\"timestamp\":\"1969-12-31 23:59:59\"") != std::string::npos);
    }
    // Tokyo has a fixed +09:00 offset (no DST), so the wall clock shifts by nine hours.
    {
        FakeConversations conversations;
        FakeEmbeddings embeddings;
        rag::RagToolProvider provider(conversations, embeddings, "Asia/Tokyo");
        conversations.conversation_count = 1;
        conversations.conversation_page = {conversation_row("c", "T", 946684800000LL)};
        const std::string result = provider.execute("list_conversations", "{}", make_ctx());
        CHECK(result.find("\"timestamp\":\"2000-01-01 09:00:00\"") != std::string::npos);
    }
}
