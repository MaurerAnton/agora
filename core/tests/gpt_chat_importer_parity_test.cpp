// Golden parity test for the GPT chat importer (graph rebuild + import format).
//
// The scenario feeds the importer the bytes of a ChatGPT export (a ZIP archive built by the Kotlin
// driver and frozen as a fixture, or the raw conversations.json array). The trace pins the preview,
// the import entities, the selected branches and the failure category.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/gpt_chat_importer.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace gpt = agora::core::gpt_import;
namespace claude = agora::core::claude_import;
namespace json = agora::core::json;

using json::Kind;
using json::Member;
using json::Value;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const Value* field(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::int64_t now_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** A timestamp inside the run window is the wall-clock fallback and is masked. */
std::string render_timestamp(std::int64_t value, std::int64_t before, std::int64_t after) {
    if (value >= before - 1000 && value <= after + 1000) return "\"<now>\"";
    return std::to_string(value);
}

std::string render_preview(const gpt::ImportPreview& preview) {
    std::string out = "{\"conversations\":[";
    for (std::size_t i = 0; i < preview.conversations.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += "{\"uuid\":";
        append_quoted(out, preview.conversations[i].uuid);
        out += ",\"title\":";
        append_quoted(out, preview.conversations[i].title);
        out += ",\"messageCount\":" + std::to_string(preview.conversations[i].message_count);
        out.push_back('}');
    }
    out += "],\"conversationCount\":" + std::to_string(preview.conversation_count);
    out += ",\"totalMessageCount\":" + std::to_string(preview.total_message_count);
    out += ",\"userMessageCount\":" + std::to_string(preview.user_message_count);
    out += ",\"assistantMessageCount\":" + std::to_string(preview.assistant_message_count);
    out += std::string(",\"hasAttachments\":") + (preview.has_attachments ? "true" : "false");
    out.push_back('}');
    return out;
}

std::string render_import(const claude::ImportConversations& imported, std::int64_t before,
                           std::int64_t after) {
    std::string out = "{\"conversations\":[";
    for (std::size_t i = 0; i < imported.conversations.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += "{\"id\":";
        append_quoted(out, imported.conversations[i].id);
        out += ",\"title\":";
        append_quoted(out, imported.conversations[i].title);
        out += ",\"lastUpdated\":" +
               render_timestamp(imported.conversations[i].last_updated, before, after);
        out += ",\"selectedBranchesJson\":";
        if (imported.conversations[i].selected_branches_json.has_value()) {
            append_quoted(out, *imported.conversations[i].selected_branches_json);
        } else {
            out += "null";
        }
        out += ",\"systemPromptId\":null,\"modelId\":null}";
    }
    out += "],\"messages\":[";
    for (std::size_t i = 0; i < imported.messages.size(); ++i) {
        if (i != 0) out.push_back(',');
        const claude::ImportMessageEntity& message = imported.messages[i];
        out += "{\"id\":";
        append_quoted(out, message.id);
        out += ",\"conversationId\":";
        append_quoted(out, message.conversation_id);
        out += ",\"parentId\":";
        if (message.parent_id.has_value()) {
            append_quoted(out, *message.parent_id);
        } else {
            out += "null";
        }
        out += ",\"text\":";
        append_quoted(out, message.text);
        out += ",\"images\":[";
        for (std::size_t k = 0; k < message.images.size(); ++k) {
            if (k != 0) out.push_back(',');
            append_quoted(out, message.images[k]);
        }
        out += "],\"thoughts\":";
        if (message.thoughts.has_value()) {
            append_quoted(out, *message.thoughts);
        } else {
            out += "null";
        }
        out += ",\"thoughtTitle\":";
        if (message.thought_title.has_value()) {
            append_quoted(out, *message.thought_title);
        } else {
            out += "null";
        }
        out += ",\"tokenCount\":" + std::to_string(message.token_count);
        out += ",\"status\":";
        append_quoted(out, message.status);
        out += ",\"participant\":";
        append_quoted(out, message.participant);
        out += ",\"timestamp\":" + render_timestamp(message.timestamp, before, after);
        out += ",\"thoughtTimeMs\":";
        if (message.thought_time_ms.has_value()) {
            out += std::to_string(*message.thought_time_ms);
        } else {
            out += "null";
        }
        out += ",\"modelName\":";
        if (message.model_name.has_value()) {
            append_quoted(out, *message.model_name);
        } else {
            out += "null";
        }
        out += ",\"toolCallJson\":";
        if (message.tool_call_json.has_value()) {
            append_quoted(out, *message.tool_call_json);
        } else {
            out += "null";
        }
        out += ",\"attachmentMeta\":null}";
    }
    out += "]}";
    return out;
}

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;

    std::string bytes;
    if (field(root, "zip") != nullptr) {
        const std::filesystem::path fixture =
            path.parent_path().parent_path() / "fixtures" / (path.stem().string() + ".zip");
        bytes = read_file(fixture);
    } else if (const Value* text = field(root, "text"); text != nullptr) {
        if (text->kind != Kind::String) scenario_fail("`text` must be a string");
        bytes = text->text;
    } else {
        scenario_fail("scenario needs `zip` or `text`");
    }

    std::optional<std::vector<std::string>> selected_ids;
    if (const Value* selected = field(root, "selectedIds"); selected != nullptr) {
        if (selected->kind != Kind::Array) scenario_fail("selectedIds must be an array");
        std::vector<std::string> ids;
        for (const Value& element : selected->elements) {
            if (element.kind != Kind::String) scenario_fail("selectedIds must be strings");
            ids.push_back(element.text);
        }
        selected_ids = ids;
    }

    const std::int64_t before = now_millis();
    const gpt::Extraction extraction = gpt::extract_and_parse(bytes);
    const std::int64_t after = now_millis();

    if (!extraction.ok) {
        std::string out = "{\"error\":{\"type\":";
        append_quoted(out, extraction.failure.type);
        if (extraction.failure.type != "decode") {
            out += ",\"message\":";
            append_quoted(out, extraction.failure.message);
        }
        out += "}}";
        return out;
    }
    const gpt::ImportPreview preview = gpt::preview(extraction.conversations);
    const claude::ImportConversations imported =
        gpt::to_import_format(extraction.conversations, selected_ids);
    return "{\"preview\":" + render_preview(preview) +
           ",\"import\":" + render_import(imported, before, after) + "}";
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 80 ? at - 80 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(320, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << ", expected "
            << expected.size() << ")\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(gpt_chat_importer_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_GPT_CHAT_IMPORTER_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
            expected.pop_back();
        }
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
