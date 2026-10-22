// Golden parity test for the attachment clone session.
//
// The test drives the same real files as the Kotlin stand: the clone copies them, the metadata
// rewrite is compared, and the rollback is observed on the file system.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/attachment_clone_session.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace clone = agora::core::attachment_clone;
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

std::string required_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

std::optional<std::string> optional_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::vector<std::string> string_list(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    std::vector<std::string> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) scenario_fail(std::string(name) + " must be strings");
        out.push_back(element.text);
    }
    return out;
}

/** The real file system, so the copy and the rollback are observed. */
class LocalFileStore : public clone::FileStore {
public:
    bool is_file(const std::string& path) const override {
        std::error_code code;
        return std::filesystem::is_regular_file(path, code);
    }
    bool directory_exists(const std::string& path) const override {
        std::error_code code;
        return std::filesystem::is_directory(path, code);
    }
    bool create_directories(const std::string& path) override {
        std::error_code code;
        return std::filesystem::create_directories(path, code);
    }
    bool copy_file(const std::string& source, const std::string& destination) override {
        std::error_code code;
        std::filesystem::copy_file(source, destination,
                                   std::filesystem::copy_options::none, code);
        return !code;
    }
    bool delete_file(const std::string& path) override {
        std::error_code code;
        return std::filesystem::remove(path, code);
    }
    std::string absolute_path(const std::string& path) const override {
        std::error_code code;
        return std::filesystem::absolute(path, code).string();
    }
};

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;

    const std::filesystem::path workspace = "platform_clone_workspace";
    std::error_code code;
    std::filesystem::remove_all(workspace, code);
    std::filesystem::create_directories(workspace / "sources", code);
    const std::filesystem::path destination = workspace / "clones";

    if (const Value* sources = field(root, "sources");
        sources != nullptr && sources->kind == Kind::Array) {
        for (const Value& element : sources->elements) {
            const std::string name = required_string(element, "name");
            const std::string content = optional_string(element, "content").value_or("");
            std::ofstream file(workspace / "sources" / name, std::ios::binary);
            file << content;
        }
    }

    LocalFileStore files;
    int generated = 0;
    clone::CloneSession session(files, destination.string(), [&generated]() {
        ++generated;
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer),
                      "00000000-0000-4000-8000-%012d", generated);
        return std::string(buffer);
    });

    const std::string workspace_prefix = std::filesystem::absolute(workspace).string();
    const std::regex uuid_pattern(
        "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    const auto mask = [&](const std::string& value) {
        std::string masked = value;
        const std::size_t at = masked.find(workspace_prefix);
        if (at != std::string::npos) masked.replace(at, workspace_prefix.size(), "<workspace>");
        return std::regex_replace(masked, uuid_pattern, "<uuid>");
    };

    std::string trace = "{\"clones\":[";
    if (const Value* messages = field(root, "messages");
        messages != nullptr && messages->kind == Kind::Array) {
        bool first = true;
        for (const Value& element : messages->elements) {
            clone::CloneableMessage message;
            message.id = required_string(element, "id");
            message.images = string_list(element, "images");
            message.attachment_meta = optional_string(element, "attachmentMeta");
            const std::optional<std::string> owner = optional_string(element, "ownerKey");
            const clone::CloneableMessage cloned = session.clone_message(message, owner);
            if (!first) trace.push_back(',');
            first = false;
            trace += "{\"id\":";
            append_quoted(trace, cloned.id);
            trace += ",\"images\":[";
            for (std::size_t i = 0; i < cloned.images.size(); ++i) {
                if (i != 0) trace.push_back(',');
                append_quoted(trace, mask(cloned.images[i]));
            }
            trace += "],\"attachmentMeta\":";
            if (cloned.attachment_meta.has_value()) {
                append_quoted(trace, mask(*cloned.attachment_meta));
            } else {
                trace += "null";
            }
            trace.push_back('}');
        }
    }
    if (const Value* standalone = field(root, "standaloneMeta"); standalone != nullptr) {
        const std::string raw = required_string(*standalone, "raw");
        const std::optional<std::string> cloned = clone::clone_attachment_meta(
            raw, [&session](const std::string& path) {
                return session.clone_backing_path("owner", path);
            });
        trace += (trace.size() > 12 ? "," : "");
        trace += "{\"standaloneMeta\":";
        append_quoted(trace, cloned.has_value() ? mask(*cloned) : std::string());
        trace.push_back('}');
    }

    const bool commit = optional_string(root, "commit").value_or("") == "true";
    if (commit) {
        session.commit();
    } else {
        session.rollback();
    }

    trace += "],\"cloneDirectoryExists\":" +
             std::string(std::filesystem::exists(destination) ? "true" : "false") +
             ",\"cloneFiles\":[";
    bool first_file = true;
    if (std::filesystem::exists(destination)) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(destination, code)) {
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            if (!first_file) trace.push_back(',');
            first_file = false;
            trace += "{\"name\":";
            append_quoted(trace, mask(file.filename().string()));
            trace += ",\"content\":";
            append_quoted(trace, read_file(file));
            trace.push_back('}');
        }
    }
    trace += "],\"sourceFiles\":[";
    bool first_source = true;
    for (const auto& entry : std::filesystem::directory_iterator(workspace / "sources", code)) {
        if (!first_source) trace.push_back(',');
        first_source = false;
        trace += "{\"name\":";
        append_quoted(trace, entry.path().filename().string());
        trace += ",\"content\":";
        append_quoted(trace, read_file(entry.path()));
        trace.push_back('}');
    }
    trace += "]}";

    std::filesystem::remove_all(workspace, code);
    return trace;
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

TEST(attachment_clone_session_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_ATTACHMENT_CLONE_SESSION_DIR;
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
