// Golden parity test for data/NativeBackupFormat.kt.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/native_backup_format.hpp"
#include "check.hpp"

namespace {

namespace backup = agora::core::native_backup;
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

std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

[[noreturn]] void fail(const std::string& message) {
    throw agora::core::test::Failure(message);
}

const Value* field(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const Member& member : object.members) {
        if (member.key == name) found = &member.value;
    }
    return found;
}

std::string text(const Value& object, std::string_view name, std::string fallback = {}) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::String) fail(std::string(name) + " must be a string");
    return value->text;
}

int integer(const Value& object, std::string_view name, int fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    try {
        return std::stoi(value->text);
    } catch (const std::exception&) {
        fail(std::string(name) + " must be an int");
    }
}

bool boolean(const Value& object, std::string_view name, bool fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    return value->text == "true";
}

const Value& object_or_empty(const Value& object, std::string_view name) {
    static const Value empty = [] {
        Value value;
        value.kind = Kind::Object;
        return value;
    }();
    const Value* value = field(object, name);
    if (value == nullptr) return empty;
    if (value->kind != Kind::Object) fail(std::string(name) + " must be an object");
    return *value;
}

const Value& array_or_empty(const Value& object, std::string_view name) {
    static const Value empty = [] {
        Value value;
        value.kind = Kind::Array;
        return value;
    }();
    const Value* value = field(object, name);
    if (value == nullptr) return empty;
    if (value->kind != Kind::Array) fail(std::string(name) + " must be an array");
    return *value;
}

backup::StringMap string_map(const Value& object, std::string_view name) {
    backup::StringMap entries;
    for (const Member& member : object_or_empty(object, name).members) {
        if (member.value.kind != Kind::String) fail(std::string(name) + " must contain strings");
        entries.emplace_back(member.key, member.value.text);
    }
    return entries;
}

Value string_value(std::string text) {
    Value value;
    value.kind = Kind::String;
    value.text = std::move(text);
    return value;
}

Value bool_value(bool value) {
    Value literal;
    literal.kind = Kind::Literal;
    literal.text = value ? "true" : "false";
    return literal;
}

Value int_value(int value) {
    Value literal;
    literal.kind = Kind::Literal;
    literal.text = std::to_string(value);
    return literal;
}

Value array_value() {
    Value value;
    value.kind = Kind::Array;
    return value;
}

Value object_value() {
    Value value;
    value.kind = Kind::Object;
    return value;
}

void add(Value& object, std::string key, Value value) {
    object.members.push_back(Member{std::move(key), std::move(value)});
}

Value map_value(const backup::StringMap& entries) {
    Value object = object_value();
    for (const auto& [key, value] : entries) add(object, key, string_value(value));
    return object;
}

Value format_value() {
    Value object = object_value();
    add(object, "currentVersion", int_value(backup::kCurrentVersion));
    add(object, "minSupportedVersion", int_value(backup::kMinSupportedVersion));
    add(object, "separateSecretsSinceVersion", int_value(backup::kSeparateSecretsSinceVersion));
    add(object, "incrementalBaselineRevision", int_value(backup::kIncrementalBaselineRevision));
    add(object, "manifestEntry", string_value(std::string(backup::kManifestEntry)));
    add(object, "conversationsEntry", string_value(std::string(backup::kConversationsEntry)));
    add(object, "conversationIndexEntry", string_value(std::string(backup::kConversationIndexEntry)));
    add(object, "conversationEntryPrefix", string_value(std::string(backup::kConversationEntryPrefix)));
    add(object, "tasksEntry", string_value(std::string(backup::kTasksEntry)));
    add(object, "settingsEntry", string_value(std::string(backup::kSettingsEntry)));
    add(object, "legacyExtraSettingsEntry", string_value(std::string(backup::kLegacyExtraSettingsEntry)));
    add(object, "secretsEntry", string_value(std::string(backup::kSecretsEntry)));
    add(object, "systemPromptsEntry", string_value(std::string(backup::kSystemPromptsEntry)));
    add(object, "customFontEntry", string_value(std::string(backup::kCustomFontEntry)));
    add(object, "imageMediaPrefix", string_value(std::string(backup::kImageMediaPrefix)));
    add(object, "videoMediaPrefix", string_value(std::string(backup::kVideoMediaPrefix)));
    add(object, "draftMediaPrefix", string_value(std::string(backup::kDraftMediaPrefix)));
    Value supported = array_value();
    for (int version = -2; version <= 7; ++version) {
        supported.elements.push_back(bool_value(backup::is_supported(version)));
    }
    add(object, "supported", std::move(supported));
    return object;
}

backup::ShellDeviceConfig parse_shell(const Value& object) {
    backup::ShellDeviceConfig shell;
    shell.id = text(object, "id");
    shell.name = text(object, "name");
    shell.description = text(object, "description");
    shell.type = text(object, "type", "conch");
    shell.server_url = text(object, "serverUrl");
    shell.api_key = text(object, "apiKey");
    shell.conch_public_key = text(object, "conchPublicKey");
    shell.ssh_host = text(object, "sshHost");
    shell.ssh_port = integer(object, "sshPort", 22);
    shell.ssh_user = text(object, "sshUser", "root");
    shell.ssh_password = text(object, "sshPassword");
    shell.ssh_host_key = text(object, "sshHostKey");
    return shell;
}

backup::McpServerConfig parse_mcp(const Value& object) {
    backup::McpServerConfig server;
    server.id = text(object, "id");
    server.name = text(object, "name");
    server.enabled = boolean(object, "enabled", true);
    server.url = text(object, "url");
    server.transport = text(object, "transport", "STREAMABLE_HTTP");
    server.headers = string_map(object, "headers");
    return server;
}

backup::EmbeddingModelConfig parse_embedding(const Value& object) {
    backup::EmbeddingModelConfig model;
    model.id = text(object, "id");
    model.name = text(object, "name");
    model.type = text(object, "type", "REMOTE") == "LOCAL" ? backup::EmbeddingModelType::Local
                                                          : backup::EmbeddingModelType::Remote;
    model.remote_api_key = text(object, "remoteApiKey");
    model.local_file_path = text(object, "localFilePath");
    return model;
}

Value models_value(const Value& settings) {
    Value object = object_value();
    const backup::ShellDeviceConfig shell =
        backup::without_secrets(parse_shell(object_or_empty(settings, "shell")));
    Value shell_value = object_value();
    add(shell_value, "id", string_value(shell.id));
    add(shell_value, "name", string_value(shell.name));
    add(shell_value, "apiKey", string_value(shell.api_key));
    add(shell_value, "sshPassword", string_value(shell.ssh_password));
    add(object, "shell", std::move(shell_value));

    const backup::McpServerConfig mcp =
        backup::without_secrets(parse_mcp(object_or_empty(settings, "mcp")));
    Value mcp_value = object_value();
    add(mcp_value, "id", string_value(mcp.id));
    add(mcp_value, "headers", map_value(mcp.headers));
    add(object, "mcp", std::move(mcp_value));

    const std::optional<backup::EmbeddingModelConfig> portable =
        backup::as_portable_remote_config(parse_embedding(object_or_empty(settings, "embedding")));
    if (!portable) {
        Value null;
        null.kind = Kind::Literal;
        null.text = "null";
        add(object, "embedding", std::move(null));
    } else {
        Value embedding = object_value();
        add(embedding, "id", string_value(portable->id));
        add(embedding, "type", string_value(portable->type == backup::EmbeddingModelType::Local
                                                 ? "LOCAL"
                                                 : "REMOTE"));
        add(embedding, "remoteApiKey", string_value(portable->remote_api_key));
        add(embedding, "localFilePath", string_value(portable->local_file_path));
        add(object, "embedding", std::move(embedding));
    }
    return object;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) fail(stem + ": not an object");
    const Value& root = *parsed.value;
    const std::string name = text(root, "name", stem);
    const std::string op = text(root, "op");

    Value result;
    if (op == "format") {
        result = format_value();
    } else if (op == "entry") {
        result = array_value();
        for (const Value& id : array_or_empty(root, "ids").elements) {
            if (id.kind != Kind::String) fail("ids must contain strings");
            result.elements.push_back(string_value(backup::conversation_entry(id.text)));
        }
    } else if (op == "models") {
        result = models_value(object_or_empty(root, "settings"));
    } else {
        fail(stem + ": unknown op " + op);
    }

    return "name|" + esc(name) + "\nop|" + esc(op) + "\nresult|" +
           esc(json::to_kotlin_string(result)) + "|-\n";
}

}  // namespace

TEST(native_backup_format_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_NATIVE_BACKUP_FORMAT_DIR;
    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(root / "scenarios")) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());
    for (const auto& scenario : scenarios) {
        const std::string expected = read_file(root / "golden" / (scenario.stem().string() + ".trace"));
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": trace mismatch\n" +
                                             actual + "\nexpected:\n" + expected);
        }
    }
}
