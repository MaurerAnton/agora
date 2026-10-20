// Port of data/NativeBackupSecretsPolicy.kt.
//
// The policy is pure settings bookkeeping: capture copies secrets out of the settings flows, and
// restore merges or replaces them back. Kotlin's LinkedHashMap order is observable in the backup
// payload and the restored settings, so every map is an insertion-ordered vector of pairs here.
#include "agora/core/native_backup_secrets.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>

#include "agora/core/json.hpp"

namespace agora::core::native_backup {
namespace {

template <typename V>
const V* lookup(const std::vector<std::pair<std::string, V>>& entries, std::string_view key) {
    for (const auto& entry : entries) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

/** LinkedHashMap put: an existing key keeps its position and receives the new value. */
template <typename V>
void put(std::vector<std::pair<std::string, V>>& entries, const std::string& key, V value) {
    for (auto& entry : entries) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    entries.emplace_back(key, std::move(value));
}

StringMap merge_maps(const StringMap& left, const StringMap& right) {
    StringMap merged = left;
    for (const auto& [key, value] : right) put(merged, key, value);
    return merged;
}

bool not_blank(std::string_view value) { return !json::is_blank(value); }

template <typename V>
bool contains_key(const std::vector<std::pair<std::string, V>>& entries, std::string_view key) {
    return lookup(entries, key) != nullptr;
}

}  // namespace

NativeBackupSecrets capture(const SettingsStore& settings) {
    NativeBackupSecrets secrets;

    for (const ShellDeviceConfig& device : settings.shell_devices()) {
        if (not_blank(device.api_key) || not_blank(device.ssh_password)) {
            put(secrets.shell_devices, device.id,
                ShellDeviceSecrets{device.api_key, device.ssh_password});
        }
    }

    for (const EmbeddingModelConfig& model : settings.embedding_models()) {
        if (model.type == EmbeddingModelType::Remote && not_blank(model.remote_api_key)) {
            put(secrets.embedding_api_keys, model.id, model.remote_api_key);
        }
    }

    for (const McpServerConfig& server : settings.mcp_servers()) {
        if (!server.headers.empty()) put(secrets.mcp_headers, server.id, server.headers);
    }

    secrets.api_keys = settings.api_keys();
    secrets.active_api_key_ids = settings.active_api_key_ids();
    secrets.web_search_api_keys = settings.web_search_api_keys();
    secrets.proxy_password = settings.proxy_password();
    return secrets;
}

RestoreResult restore(const NativeBackupSecrets& data, SettingsStore& settings, bool replace) {
    RestoreResult result;
    StringMap imported_id_map;

    std::vector<ApiKeyEntry> final_keys;
    if (replace) {
        final_keys = data.api_keys;
    } else {
        final_keys = settings.api_keys();
        for (const ApiKeyEntry& imported : data.api_keys) {
            const auto same_id = std::find_if(
                final_keys.begin(), final_keys.end(),
                [&](const ApiKeyEntry& entry) { return entry.id == imported.id; });
            if (same_id != final_keys.end()) {
                *same_id = imported;
                put(imported_id_map, imported.id, imported.id);
                continue;
            }
            const auto semantic = std::find_if(
                final_keys.begin(), final_keys.end(), [&](const ApiKeyEntry& entry) {
                    return entry.provider == imported.provider && entry.key == imported.key;
                });
            if (semantic != final_keys.end()) {
                put(imported_id_map, imported.id, semantic->id);
            } else {
                final_keys.push_back(imported);
                put(imported_id_map, imported.id, imported.id);
            }
        }
    }
    if (replace) {
        for (const ApiKeyEntry& imported : data.api_keys) {
            put(imported_id_map, imported.id, imported.id);
        }
    }
    settings.save_api_keys(final_keys);

    StringMap imported_active_ids;
    for (const auto& [slot, imported_id] : data.active_api_key_ids) {
        const std::string* mapped = lookup(imported_id_map, imported_id);
        const std::string final_id = mapped != nullptr ? *mapped : imported_id;
        const bool present = std::any_of(
            final_keys.begin(), final_keys.end(),
            [&](const ApiKeyEntry& entry) { return entry.id == final_id; });
        if (present) put(imported_active_ids, slot, final_id);
    }
    settings.save_active_api_key_ids(
        replace ? imported_active_ids : merge_maps(settings.active_api_key_ids(), imported_active_ids));

    settings.save_web_search_api_keys(
        replace ? data.web_search_api_keys
                : merge_maps(settings.web_search_api_keys(), data.web_search_api_keys));

    if (replace || not_blank(data.proxy_password)) {
        settings.save_proxy_password(data.proxy_password);
    }

    std::vector<ShellDeviceConfig> restored_shell;
    restored_shell.reserve(settings.shell_devices().size());
    std::size_t orphan_shell_secrets = 0;
    for (const ShellDeviceConfig& device : settings.shell_devices()) {
        ShellDeviceConfig restored = device;
        const ShellDeviceSecrets* by_id = lookup(data.shell_devices, device.id);
        const std::string* legacy_api_key = lookup(data.shell_api_keys, device.name);
        if (by_id != nullptr) {
            restored.api_key = by_id->api_key;
            restored.ssh_password = by_id->ssh_password;
        } else if (legacy_api_key != nullptr) {
            restored.api_key = *legacy_api_key;
            if (replace) restored.ssh_password.clear();
        } else if (replace) {
            restored.api_key.clear();
            restored.ssh_password.clear();
        }
        restored_shell.push_back(std::move(restored));
    }
    for (const auto& [id, unused] : data.shell_devices) {
        const bool matched = std::any_of(
            settings.shell_devices().begin(), settings.shell_devices().end(),
            [&](const ShellDeviceConfig& device) { return device.id == id; });
        if (!matched) ++orphan_shell_secrets;
    }
    for (const auto& [name, unused] : data.shell_api_keys) {
        const bool matched = std::any_of(
            settings.shell_devices().begin(), settings.shell_devices().end(),
            [&](const ShellDeviceConfig& device) { return device.name == name; });
        if (!matched) ++orphan_shell_secrets;
    }
    if (orphan_shell_secrets > 0) {
        result.warnings.push_back("ignored " + std::to_string(orphan_shell_secrets) +
                                  " shell credential record(s) without a matching device");
    }
    settings.save_shell_devices(restored_shell);

    const std::vector<EmbeddingModelConfig>& embedding_models = settings.embedding_models();
    std::unordered_set<std::string> embedding_ids;
    for (const EmbeddingModelConfig& model : embedding_models) embedding_ids.insert(model.id);
    std::size_t orphan_embedding_secrets = 0;
    for (const auto& [id, unused] : data.embedding_api_keys) {
        if (embedding_ids.count(id) == 0) ++orphan_embedding_secrets;
    }
    if (orphan_embedding_secrets > 0) {
        result.warnings.push_back("ignored " + std::to_string(orphan_embedding_secrets) +
                                  " embedding credential record(s) without a matching model");
    }
    std::vector<EmbeddingModelConfig> restored_embeddings;
    restored_embeddings.reserve(embedding_models.size());
    for (const EmbeddingModelConfig& model : embedding_models) {
        EmbeddingModelConfig restored = model;
        if (restored.type == EmbeddingModelType::Remote) {
            const std::string* imported = lookup(data.embedding_api_keys, model.id);
            if (imported != nullptr) {
                restored.remote_api_key = *imported;
            } else if (replace) {
                restored.remote_api_key.clear();
            }
        }
        restored_embeddings.push_back(std::move(restored));
    }
    settings.save_embedding_models(restored_embeddings);

    const std::vector<McpServerConfig>& mcp_servers = settings.mcp_servers();
    std::unordered_set<std::string> mcp_ids;
    for (const McpServerConfig& server : mcp_servers) mcp_ids.insert(server.id);
    std::size_t orphan_mcp_secrets = 0;
    for (const auto& [id, unused] : data.mcp_headers) {
        if (mcp_ids.count(id) == 0) ++orphan_mcp_secrets;
    }
    if (orphan_mcp_secrets > 0) {
        result.warnings.push_back("ignored " + std::to_string(orphan_mcp_secrets) +
                                  " MCP header record(s) without a matching server");
    }
    std::vector<McpServerConfig> restored_mcp;
    restored_mcp.reserve(mcp_servers.size());
    for (const McpServerConfig& server : mcp_servers) {
        McpServerConfig restored = server;
        const StringMap* imported = lookup(data.mcp_headers, server.id);
        if (imported != nullptr) {
            restored.headers = *imported;
        } else if (replace) {
            restored.headers.clear();
        }
        restored_mcp.push_back(std::move(restored));
    }
    settings.save_mcp_servers(restored_mcp);

    return result;
}

}  // namespace agora::core::native_backup
