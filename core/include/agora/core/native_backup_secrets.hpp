#pragma once

#include <string>
#include <utility>
#include <vector>

namespace agora::core::native_backup {

// Models shared with the app's settings contracts (data/SettingsContracts.kt,
// data/EmbeddingModelConfig.kt) and the native backup format
// (data/NativeBackupFormat.kt). Only the fields the secrets policy observes are represented.
struct ApiKeyEntry {
    std::string id;
    std::string name;
    std::string key;
    std::string provider;
};

struct ShellDeviceConfig {
    std::string id;
    std::string name;
    std::string description;
    std::string type = "conch";
    std::string server_url;
    std::string api_key;
    std::string conch_public_key;
    std::string ssh_host;
    int ssh_port = 22;
    std::string ssh_user = "root";
    std::string ssh_password;
    std::string ssh_host_key;
};

enum class EmbeddingModelType { Remote, Local };

struct EmbeddingModelConfig {
    std::string id;
    std::string name;
    EmbeddingModelType type = EmbeddingModelType::Remote;
    std::string remote_model_name;
    std::string remote_base_url;
    std::string remote_api_key;
    std::string local_file_path;
    int batch_size = 8;
};

using StringMap = std::vector<std::pair<std::string, std::string>>;

struct McpServerConfig {
    std::string id;
    std::string name;
    bool enabled = true;
    std::string url;
    std::string transport = "streamable_http";
    StringMap headers;
    std::vector<std::string> disabled_tools;
};

struct ShellDeviceSecrets {
    std::string api_key;
    std::string ssh_password;
};

/** data/NativeBackupFormat.kt: NativeBackupSecrets. Maps keep insertion order like LinkedHashMap. */
struct NativeBackupSecrets {
    std::vector<ApiKeyEntry> api_keys;
    StringMap active_api_key_ids;
    StringMap web_search_api_keys;
    std::string proxy_password;
    std::vector<std::pair<std::string, ShellDeviceSecrets>> shell_devices;
    StringMap shell_api_keys;
    StringMap embedding_api_keys;
    std::vector<std::pair<std::string, StringMap>> mcp_headers;
};

/** The SettingsManager read/write surface used by the policy. */
class SettingsStore {
public:
    virtual ~SettingsStore() = default;

    virtual const std::vector<ApiKeyEntry>& api_keys() const = 0;
    virtual const StringMap& active_api_key_ids() const = 0;
    virtual const StringMap& web_search_api_keys() const = 0;
    virtual const std::string& proxy_password() const = 0;
    virtual const std::vector<ShellDeviceConfig>& shell_devices() const = 0;
    virtual const std::vector<EmbeddingModelConfig>& embedding_models() const = 0;
    virtual const std::vector<McpServerConfig>& mcp_servers() const = 0;

    virtual void save_api_keys(const std::vector<ApiKeyEntry>& keys) = 0;
    virtual void save_active_api_key_ids(const StringMap& ids) = 0;
    virtual void save_web_search_api_keys(const StringMap& keys) = 0;
    virtual void save_proxy_password(const std::string& password) = 0;
    virtual void save_shell_devices(const std::vector<ShellDeviceConfig>& devices) = 0;
    virtual void save_embedding_models(const std::vector<EmbeddingModelConfig>& models) = 0;
    virtual void save_mcp_servers(const std::vector<McpServerConfig>& servers) = 0;
};

/** Port of NativeBackupSecretsPolicy.capture. */
NativeBackupSecrets capture(const SettingsStore& settings);

struct RestoreResult {
    std::vector<std::string> warnings;
};

/** Port of NativeBackupSecretsPolicy.restore. */
RestoreResult restore(const NativeBackupSecrets& data, SettingsStore& settings, bool replace);

}  // namespace agora::core::native_backup
