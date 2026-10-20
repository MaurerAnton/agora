// Golden parity test for NativeBackupSecretsPolicy.
//
// The scenario seeds an in-memory SettingsStore; the C++ capture/restore must reproduce the Kotlin
// trace exactly: insertion order of every map, warning order, cleared-vs-preserved secrets and the
// complete restored settings snapshot.
#include <algorithm>
#include <cstddef>
#include <cstdint>
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
#include "agora/core/native_backup_secrets.hpp"
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

const Value& required_object(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Object) {
        scenario_fail(std::string(name) + " must be an object");
    }
    return *value;
}

std::string text_or(const Value& object, std::string_view name, std::string fallback = {}) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

int int_or(const Value& object, std::string_view name, int fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be an int");
    try {
        return std::stoi(value->text);
    } catch (const std::exception&) {
        scenario_fail(std::string(name) + " is not an int");
    }
}

bool bool_or(const Value& object, std::string_view name, bool fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a bool");
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    scenario_fail(std::string(name) + " is not a bool");
}

const Value& array_or_empty(const Value& object, std::string_view name) {
    static const Value empty = [] {
        Value value;
        value.kind = Kind::Array;
        return value;
    }();
    const Value* value = field(object, name);
    if (value == nullptr) return empty;
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    return *value;
}

const Value& object_or_empty(const Value& object, std::string_view name) {
    static const Value empty = [] {
        Value value;
        value.kind = Kind::Object;
        return value;
    }();
    const Value* value = field(object, name);
    if (value == nullptr) return empty;
    if (value->kind != Kind::Object) scenario_fail(std::string(name) + " must be an object");
    return *value;
}

backup::StringMap string_map(const Value& object, std::string_view name) {
    backup::StringMap entries;
    for (const Member& member : object_or_empty(object, name).members) {
        if (member.value.kind != Kind::String) {
            scenario_fail(std::string(name) + "." + member.key + " must be a string");
        }
        entries.emplace_back(member.key, member.value.text);
    }
    return entries;
}

backup::ApiKeyEntry parse_api_key(const Value& value) {
    return backup::ApiKeyEntry{
        text_or(value, "id"),
        text_or(value, "name"),
        text_or(value, "key"),
        text_or(value, "provider", "Google"),
    };
}

backup::ShellDeviceConfig parse_shell_device(const Value& value) {
    backup::ShellDeviceConfig device;
    device.id = text_or(value, "id");
    device.name = text_or(value, "name");
    device.description = text_or(value, "description");
    device.type = text_or(value, "type", "conch");
    device.server_url = text_or(value, "serverUrl");
    device.api_key = text_or(value, "apiKey");
    device.conch_public_key = text_or(value, "conchPublicKey");
    device.ssh_host = text_or(value, "sshHost");
    device.ssh_port = int_or(value, "sshPort", 22);
    device.ssh_user = text_or(value, "sshUser", "root");
    device.ssh_password = text_or(value, "sshPassword");
    device.ssh_host_key = text_or(value, "sshHostKey");
    return device;
}

backup::EmbeddingModelConfig parse_embedding_model(const Value& value) {
    backup::EmbeddingModelConfig model;
    model.id = text_or(value, "id");
    model.name = text_or(value, "name");
    const std::string type = text_or(value, "type", "REMOTE");
    model.type = type == "LOCAL" ? backup::EmbeddingModelType::Local
                                 : backup::EmbeddingModelType::Remote;
    model.remote_model_name = text_or(value, "remoteModelName");
    model.remote_base_url = text_or(value, "remoteBaseUrl");
    model.remote_api_key = text_or(value, "remoteApiKey");
    model.local_file_path = text_or(value, "localFilePath");
    model.batch_size = int_or(value, "batchSize", 8);
    return model;
}

backup::McpServerConfig parse_mcp_server(const Value& value) {
    backup::McpServerConfig server;
    server.id = text_or(value, "id");
    server.name = text_or(value, "name");
    server.enabled = bool_or(value, "enabled", true);
    server.url = text_or(value, "url");
    server.transport = text_or(value, "transport", "STREAMABLE_HTTP");
    server.headers = string_map(value, "headers");
    for (const Value& tool : array_or_empty(value, "disabledTools").elements) {
        if (tool.kind != Kind::String) scenario_fail("disabledTools must contain strings");
        server.disabled_tools.push_back(tool.text);
    }
    return server;
}

class FakeSettings : public backup::SettingsStore {
public:
    std::vector<backup::ApiKeyEntry> api_keys_;
    backup::StringMap active_api_key_ids_;
    backup::StringMap web_search_api_keys_;
    std::string proxy_password_;
    std::vector<backup::ShellDeviceConfig> shell_devices_;
    std::vector<backup::EmbeddingModelConfig> embedding_models_;
    std::vector<backup::McpServerConfig> mcp_servers_;

    const std::vector<backup::ApiKeyEntry>& api_keys() const override { return api_keys_; }
    const backup::StringMap& active_api_key_ids() const override { return active_api_key_ids_; }
    const backup::StringMap& web_search_api_keys() const override { return web_search_api_keys_; }
    const std::string& proxy_password() const override { return proxy_password_; }
    const std::vector<backup::ShellDeviceConfig>& shell_devices() const override {
        return shell_devices_;
    }
    const std::vector<backup::EmbeddingModelConfig>& embedding_models() const override {
        return embedding_models_;
    }
    const std::vector<backup::McpServerConfig>& mcp_servers() const override { return mcp_servers_; }

    void save_api_keys(const std::vector<backup::ApiKeyEntry>& keys) override { api_keys_ = keys; }
    void save_active_api_key_ids(const backup::StringMap& ids) override {
        active_api_key_ids_ = ids;
    }
    void save_web_search_api_keys(const backup::StringMap& keys) override {
        web_search_api_keys_ = keys;
    }
    void save_proxy_password(const std::string& password) override { proxy_password_ = password; }
    void save_shell_devices(const std::vector<backup::ShellDeviceConfig>& devices) override {
        shell_devices_ = devices;
    }
    void save_embedding_models(const std::vector<backup::EmbeddingModelConfig>& models) override {
        embedding_models_ = models;
    }
    void save_mcp_servers(const std::vector<backup::McpServerConfig>& servers) override {
        mcp_servers_ = servers;
    }
};

Value string_value(std::string text) {
    Value value;
    value.kind = Kind::String;
    value.text = std::move(text);
    return value;
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

Value string_map_value(const backup::StringMap& entries) {
    Value object = object_value();
    for (const auto& [key, value] : entries) add(object, key, string_value(value));
    return object;
}

Value api_keys_value(const std::vector<backup::ApiKeyEntry>& keys) {
    Value array = array_value();
    for (const backup::ApiKeyEntry& entry : keys) {
        Value object = object_value();
        add(object, "id", string_value(entry.id));
        add(object, "name", string_value(entry.name));
        add(object, "key", string_value(entry.key));
        add(object, "provider", string_value(entry.provider));
        array.elements.push_back(std::move(object));
    }
    return array;
}

Value secrets_value(const backup::NativeBackupSecrets& secrets) {
    Value object = object_value();
    add(object, "apiKeys", api_keys_value(secrets.api_keys));
    add(object, "activeApiKeyIds", string_map_value(secrets.active_api_key_ids));
    add(object, "webSearchApiKeys", string_map_value(secrets.web_search_api_keys));
    add(object, "proxyPassword", string_value(secrets.proxy_password));
    Value shell_devices = object_value();
    for (const auto& [id, entry] : secrets.shell_devices) {
        Value shell = object_value();
        add(shell, "apiKey", string_value(entry.api_key));
        add(shell, "sshPassword", string_value(entry.ssh_password));
        add(shell_devices, id, std::move(shell));
    }
    add(object, "shellDevices", std::move(shell_devices));
    add(object, "shellApiKeys", string_map_value(secrets.shell_api_keys));
    add(object, "embeddingApiKeys", string_map_value(secrets.embedding_api_keys));
    Value mcp_headers = object_value();
    for (const auto& [id, headers] : secrets.mcp_headers) {
        add(mcp_headers, id, string_map_value(headers));
    }
    add(object, "mcpHeaders", std::move(mcp_headers));
    return object;
}

Value settings_value(const FakeSettings& settings) {
    Value object = object_value();
    add(object, "apiKeys", api_keys_value(settings.api_keys_));
    add(object, "activeApiKeyIds", string_map_value(settings.active_api_key_ids_));
    add(object, "webSearchApiKeys", string_map_value(settings.web_search_api_keys_));
    add(object, "proxyPassword", string_value(settings.proxy_password_));
    Value shell_devices = array_value();
    for (const backup::ShellDeviceConfig& device : settings.shell_devices_) {
        Value shell = object_value();
        add(shell, "id", string_value(device.id));
        add(shell, "name", string_value(device.name));
        add(shell, "apiKey", string_value(device.api_key));
        add(shell, "sshPassword", string_value(device.ssh_password));
        shell_devices.elements.push_back(std::move(shell));
    }
    add(object, "shellDevices", std::move(shell_devices));
    Value embeddings = array_value();
    for (const backup::EmbeddingModelConfig& model : settings.embedding_models_) {
        Value embedding = object_value();
        add(embedding, "id", string_value(model.id));
        add(embedding, "type",
            string_value(model.type == backup::EmbeddingModelType::Local ? "LOCAL" : "REMOTE"));
        add(embedding, "remoteApiKey", string_value(model.remote_api_key));
        embeddings.elements.push_back(std::move(embedding));
    }
    add(object, "embeddingModels", std::move(embeddings));
    Value mcp_servers = array_value();
    for (const backup::McpServerConfig& server : settings.mcp_servers_) {
        Value mcp = object_value();
        add(mcp, "id", string_value(server.id));
        add(mcp, "headers", string_map_value(server.headers));
        mcp_servers.elements.push_back(std::move(mcp));
    }
    add(object, "mcpServers", std::move(mcp_servers));
    return object;
}

Value warnings_value(const std::vector<std::string>& warnings) {
    Value array = array_value();
    for (const std::string& warning : warnings) array.elements.push_back(string_value(warning));
    return array;
}

backup::NativeBackupSecrets parse_secrets(const Value& root) {
    const Value& secrets = required_object(root, "secrets");
    backup::NativeBackupSecrets data;
    for (const Value& value : array_or_empty(secrets, "apiKeys").elements) {
        data.api_keys.push_back(parse_api_key(value));
    }
    data.active_api_key_ids = string_map(secrets, "activeApiKeyIds");
    data.web_search_api_keys = string_map(secrets, "webSearchApiKeys");
    data.proxy_password = text_or(secrets, "proxyPassword");
    for (const Member& member : object_or_empty(secrets, "shellDevices").members) {
        const Value& entry = member.value;
        data.shell_devices.emplace_back(
            member.key, backup::ShellDeviceSecrets{text_or(entry, "apiKey"),
                                                   text_or(entry, "sshPassword")});
    }
    data.shell_api_keys = string_map(secrets, "shellApiKeys");
    data.embedding_api_keys = string_map(secrets, "embeddingApiKeys");
    for (const Member& member : object_or_empty(secrets, "mcpHeaders").members) {
        backup::StringMap headers;
        for (const Member& header : member.value.members) {
            if (header.value.kind != Kind::String) {
                scenario_fail("mcpHeaders." + member.key + " must contain strings");
            }
            headers.emplace_back(header.key, header.value.text);
        }
        data.mcp_headers.emplace_back(member.key, std::move(headers));
    }
    return data;
}

FakeSettings parse_settings(const Value& root) {
    const Value& settings = required_object(root, "settings");
    FakeSettings store;
    for (const Value& value : array_or_empty(settings, "apiKeys").elements) {
        store.api_keys_.push_back(parse_api_key(value));
    }
    store.active_api_key_ids_ = string_map(settings, "activeApiKeyIds");
    store.web_search_api_keys_ = string_map(settings, "webSearchApiKeys");
    store.proxy_password_ = text_or(settings, "proxyPassword");
    for (const Value& value : array_or_empty(settings, "shellDevices").elements) {
        store.shell_devices_.push_back(parse_shell_device(value));
    }
    for (const Value& value : array_or_empty(settings, "embeddingModels").elements) {
        store.embedding_models_.push_back(parse_embedding_model(value));
    }
    for (const Value& value : array_or_empty(settings, "mcpServers").elements) {
        store.mcp_servers_.push_back(parse_mcp_server(value));
    }
    return store;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(stem + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    const std::string name = text_or(root, "name", stem);
    const std::string op = text_or(root, "op");
    FakeSettings store = parse_settings(root);

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";
    if (op == "capture") {
        const backup::NativeBackupSecrets secrets = backup::capture(store);
        trace += "result|" + esc(json::to_kotlin_string(secrets_value(secrets))) + "|-\n";
    } else if (op == "restore") {
        const backup::NativeBackupSecrets secrets = parse_secrets(root);
        const bool replace = bool_or(root, "replace", false);
        const backup::RestoreResult result = backup::restore(secrets, store, replace);
        trace += "warnings|" + esc(json::to_kotlin_string(warnings_value(result.warnings))) + "|-\n";
        trace += "result|" + esc(json::to_kotlin_string(settings_value(store))) + "|-\n";
    } else {
        scenario_fail(stem + ": unknown op " + op);
    }
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 60 ? at - 60 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(240, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << ", expected "
            << expected.size() << ")\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(native_backup_secrets_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_NATIVE_BACKUP_SECRETS_DIR;
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
        const std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
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
