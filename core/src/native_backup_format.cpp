// Port of data/NativeBackupFormat.kt.
#include "agora/core/native_backup_format.hpp"

#include <string>

namespace agora::core::native_backup {

bool is_supported(int version) {
    return version >= kMinSupportedVersion && version <= kCurrentVersion;
}

std::string conversation_entry(std::string_view id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string entry(kConversationEntryPrefix);
    entry.reserve(entry.size() + id.size() * 2 + 5);
    for (const unsigned char byte : id) {
        entry.push_back(kHex[byte >> 4]);
        entry.push_back(kHex[byte & 0x0f]);
    }
    entry += ".json";
    return entry;
}

ShellDeviceConfig without_secrets(ShellDeviceConfig device) {
    device.api_key.clear();
    device.ssh_password.clear();
    return device;
}

McpServerConfig without_secrets(McpServerConfig server) {
    server.headers.clear();
    return server;
}

std::optional<EmbeddingModelConfig> as_portable_remote_config(const EmbeddingModelConfig& model) {
    if (model.type != EmbeddingModelType::Remote) return std::nullopt;
    EmbeddingModelConfig portable = model;
    portable.remote_api_key.clear();
    portable.local_file_path.clear();
    return portable;
}

}  // namespace agora::core::native_backup
