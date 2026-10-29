# portable-settings-archive golden stand

Parity pin for `data/PortableSettingsArchive.kt` — **both** halves of the portable settings
allowlist used by native backup v4+: `toJsonObject` (the `settings.json` payload) and
`restoreFromJsonObject` + `prepareImportedCustomProviders` (the import path).

`PortableSettingsArchive.kt`, `SettingsContracts.kt`, `NativeBackupFormat.kt`,
`CustomProviderConfig.kt`, `CustomProviderNamePolicy.kt`, `SettingsDefaults.kt`,
`DefaultSystemPrompt.kt`, `SettingsPreferenceSchema.kt`, `EmbeddingModelConfig.kt`,
`ThinkingControl.kt` and `OpenAiServiceTiers.kt` compile **verbatim from the app tree**. The
identity policy inside `CustomProviderIdentity.kt` is lifted byte-for-byte by the documented
extractor of the `provider-identity` stand (`agora_extract provider-identity`).

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 110 cases: export payloads, restore payloads, accessor/decode probes, deterministic fuzz |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `PortableSettingsArchiveTraceDump.kt` | Driver: runs export/restore against the recording double |
| `platform/` | DataStore preferences keys, crypto/debug shims, recording `SettingsManager` double |
| `gen_scenarios.py` | Deterministic scenario generator (every scenario carries the full 97-key snapshot) |
| `run_golden.sh` | Extracts the identity policy, compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

**Export (`op=export`)** — the exact `Json.encodeToString` text:

* Field order is the source order of all ~95 puts, `null` for the three nullable puts.
* kotlinx `encodeDefaults=false`: a model field equal to its declared default is omitted
  (`description=""`, `type="conch"`, `sshPort=22`, `sshUser="root"`, `enabled=true`,
  `protocol="openai"`, `anthropicCacheEnabled=true`, `anthropicCacheTtl="1h"`, `batchSize=8`, …).
* `withoutSecrets()`/`asPortableRemoteConfig()` strip `apiKey`, `sshPassword`, `headers`,
  `remoteApiKey` and `localFilePath` — which is exactly why they never appear in the payload.
* `Float.toString` spelling for `ragThreshold`/`defaultTemperature`/…, StringOps escaping for
  strings, `LinkedHashSet`/`LinkedHashMap` order for arrays and maps.
* `fontPreference` collapses `custom` → `app_default` when the font file is not part of the
  export, and `customFontName` is written only when the font is included.
* `activeRemoteEmbeddingModelId` is emitted only when the id matches an exported remote model.

**Restore (`op=restore`)** — the ordered save-call log, warnings and errors:

* Accessor coercions (`string`/`nullableString`/`boolean`/`int`/`float`): numbers stringify for
  `string()`, non-primitives and `null` are absent, `Boolean` accepts case-insensitive
  `true`/`false` only, `Int` accepts a trimmed decimal literal (`" 7 "` → 7, `"7.5"` → absent),
  `Float` accepts the JVM `parseFloat` grammar. Pinned by the `70-accessor-*` probes.
* Typed decode behind `runCatching` (`71-decode-*`): a single bad element rejects the whole
  collection (falls back to empty/absent), enums need their exact serial name
  (`"REMOTE"`, `"streamable_http"` — `"remote"`/`"SSE"` fail), `CustomEndpointProtocol` is
  lenient (`claude`→`anthropic`, `gemini`→`google`, unknown→`unknown`), unknown keys are
  ignored, and `Boolean` accepts a bool-string while `Int`/`String` do not swap types.
* Merge/replace semantics: `LinkedHashMap`/`LinkedHashSet` order for `customModels`,
  `enabledModels`, `modelAliases`, `providerBaseUrls`, `imageTranscriptionEnabledModels`;
  `saveModelProviderNames` passes the `replace` flag through instead of merging; the
  `mergeById` list merges for embeddings/shell/MCP/custom providers; `resetPortableSettingsForImport`
  runs only on replace and only before the writes.
* Legacy thinking budget (`budget:<Int>`) fills the explicit budget fields only when the keys are
  absent; `thinkingLevel`/`openAiServiceTier` go through the shipped normalisers.
* Secrets: `allowLegacySecrets` gates imported `remoteApiKey`/`sshPassword`/`headers`, falling
  back to the existing record's secret; without it the previous secret wins.
* The embedding active-id dance (`activeRemoteEmbeddingModelId` → legacy `activeEmbeddingModelId`
  → previous id when it is a LOCAL model → `""`).
* The four font branches (`restoredCustomFont`, existing `File.isFile`, missing file,
  replace-clear) including `File(previousFontPath).isFile`.
* Provider preparation: reserved/duplicate names become the one warning
  `skipped invalid custom provider name(s): …`, stable ids are retained, legacy name-keyed ids
  are minted, `legacyNames` merge order, and `remapProviderReference`/`remapModelPreferenceKeys`
  rewrites for `selectedModel`, model sets, aliases, base URLs and every nullable model field.
* The three `require(...)` guards with their exact messages (`Invalid Anthropic cache duration`,
  `Invalid Anthropic cache switch`), including partial writes that stay applied before the throw.
* `setActiveSystemPromptId` for `activeSystemPromptId`, the legacy `active_system_prompt_id`,
  replace-clear, and `invalidatePortableModelCaches()` as the final call of every restore.

## Boundaries (documented, all pinned structurally)

* **SettingsManager** is the Android DataStore storage class. The `platform/SettingsManager.kt`
  double serves the 97 flows from the scenario snapshot (a missing snapshot key is an error, so
  no default is quietly reimplemented in the stand) and records save arguments verbatim. It does
  not reproduce storage-side coercions — the archive never observes them.
* **Provider-id entropy**: Kotlin mints legacy ids with `UUID.randomUUID()`. The id source is a
  parameter of the port; the driver masks any stable id that appears nowhere in the scenario
  input as `custom-provider-<generated>`, so retained/merged ids are pinned exactly and
  minted-vs-retained is pinned structurally. The minting policy itself (regex, retention,
  retry) is separately pinned by the `provider-identity` stand.
* **`resolveSystemPromptId`** is `promptImport::resolve` at the call site; the scenario supplies
  the id mapping the port is called with.
* **Model UUID defaults**: `EmbeddingModelConfig.id`, `ShellDeviceConfig.id` and
  `McpServerConfig.id` default to random UUIDs, so every snapshot supplies ids (a missing id
  would be non-deterministic, like in `native-backup-secrets`).
* **The custom-font file** is a real `java.io.File` named `platform_font.ttf` created/removed by
  the driver from the scenario's `fontFileExists` flag; the C++ test mirrors it in its own CWD.
* Protocol and boolean case folding is ASCII in the port while Kotlin uses Unicode-aware ignore
  case; all real wire values are ASCII.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
COROUTINES_JAR=/home/user/agora-toolchain/kotlinc-2.3.21/lib/kotlinx-coroutines-core-jvm.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
