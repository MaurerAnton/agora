#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "agora/core/native_backup_secrets.hpp"

namespace agora::core::native_backup {

/** data/NativeBackupFormat.kt: archive version and entry vocabulary. */
inline constexpr int kCurrentVersion = 6;
inline constexpr int kMinSupportedVersion = 1;
inline constexpr int kSeparateSecretsSinceVersion = 5;
inline constexpr int kIncrementalBaselineRevision = 1;

inline constexpr std::string_view kManifestEntry = "manifest.json";
inline constexpr std::string_view kConversationsEntry = "conversations.json";
inline constexpr std::string_view kConversationIndexEntry = "conv/index.json";
inline constexpr std::string_view kConversationEntryPrefix = "conv/items/";
inline constexpr std::string_view kTasksEntry = "conv/tasks.json";
inline constexpr std::string_view kSettingsEntry = "settings.json";
inline constexpr std::string_view kLegacyExtraSettingsEntry = "extra_settings.json";
inline constexpr std::string_view kSecretsEntry = "api_keys.json";
inline constexpr std::string_view kSystemPromptsEntry = "system_prompts.json";
inline constexpr std::string_view kCustomFontEntry = "custom_font/font";
inline constexpr std::string_view kImageMediaPrefix = "media/images/";
inline constexpr std::string_view kVideoMediaPrefix = "media/videos/";
inline constexpr std::string_view kDraftMediaPrefix = "media/drafts/";

bool is_supported(int version);
std::string conversation_entry(std::string_view id);

/** Exported settings helpers: strip credentials before writing a backup. */
ShellDeviceConfig without_secrets(ShellDeviceConfig device);
McpServerConfig without_secrets(McpServerConfig server);
std::optional<EmbeddingModelConfig> as_portable_remote_config(const EmbeddingModelConfig& model);

}  // namespace agora::core::native_backup
