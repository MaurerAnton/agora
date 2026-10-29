# native-backup-format golden stand

Parity pin for `data/NativeBackupFormat.kt`: archive version gates, entry names, conversation entry
hashing and the credential-stripping projections used by export.

The format object plus the shared settings models (`SettingsContracts.kt`, `EmbeddingModelConfig.kt`,
`PromptTemplateItem.kt`) compile **verbatim from the app tree**. Only the DataStore/crypto key
surface is stubbed, because the shared settings contract imports it.

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 5 cases: constants, entry ids, remote/local model projections |
| `golden/*.trace` | Frozen Kotlin output |
| `NativeBackupFormatTraceDump.kt` | Driver |
| `platform/` | DataStore key factory, crypto/debug shims |
| `agora_scenarios native-backup-format` | Deterministic scenario generator |
| `run_golden.sh` | Compile, freeze (`--freeze`) or verify |

## What is pinned

* All archive constants and the supported range `1..6`.
* `conversationEntry`: UTF-8 byte hex (lowercase) of the id, wrapped in `conv/items/` and `.json`.
  Cyrillic and non-BMP emoji pin multibyte encoding.
* `ShellDeviceConfig.withoutSecrets` clears the API key and SSH password.
* `McpServerConfig.withoutSecrets` clears all headers.
* `EmbeddingModelConfig.asPortableRemoteConfig`: `LOCAL` becomes `null`; `REMOTE` keeps the model
  but clears `remoteApiKey` and `localFilePath`.
