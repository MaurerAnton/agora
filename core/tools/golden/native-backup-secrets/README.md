# native-backup-secrets golden stand

Parity pin for `data/NativeBackupSecretsPolicy.kt`: the capture/restore rules that move API keys,
shell credentials, embedding keys and MCP headers in and out of a native backup.

`NativeBackupSecretsPolicy.kt`, `NativeBackupFormat.kt` (the backup data models),
`SettingsContracts.kt`, `SettingsPreferenceSchema.kt`, `PromptTemplateItem.kt` and
`EmbeddingModelConfig.kt` compile **verbatim from the app tree**.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 9 capture/restore cases |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `NativeBackupSecretsTraceDump.kt` | Driver: seeds settings, runs the policy, prints the trace |
| `platform/` | DataStore preferences keys, crypto/debug shims, in-memory `SettingsManager` |
| `agora_scenarios native-backup-secrets` | Deterministic scenario generator |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **Capture filters**: shell credentials need a non-blank API key or SSH password; embedding
  credentials need a `REMOTE` model and a non-blank key; MCP headers need a non-empty map. Kotlin
  whitespace counts as blank (`isNotBlank`).
* **Map order**: shell/embedding/MCP maps are built with `toMap`/`associate`, which keep the first
  insertion position but the last value for duplicate ids. The trace is JSON, so order is visible.
* **Replace semantics**: imported keys replace the list; active API key slots are remapped through
  the imported-id map and filtered against the final keys; web keys replace; the proxy password is
  written even when blank; shell/embedding/MCP secrets missing from the payload are cleared.
* **Merge semantics**: same-id API keys replace in place; a different id with the same
  provider+key maps onto the existing id; new ids append. Active slots are remapped and merged with
  the existing map, web keys merge with the payload overriding duplicate providers, and a blank
  proxy is not written.
* **Shell fallback**: by-id secrets win; legacy name-keyed credentials apply only without a by-id
  match, and replace clears the SSH password for legacy rows. Unmatched devices are cleared only
  on replace.
* **Orphan warnings**: shell by-id + legacy-name orphans combine into one count; embedding and MCP
  warnings follow in that order. The exact warning strings are pinned.

## Boundaries

* `SettingsManager` is the Android DataStore storage class. The policy only consumes seven flows and
  seven save methods; `platform/SettingsManager.kt` is an in-memory double with exactly that
  contract. No policy logic is reimplemented in the stand.
* The trace observes the policy's returned secrets and the restored settings. It does not pin the
  number of save calls (the final state is observable; repeated identical writes are not).
* `ApiKeyEntry.id` and other model UUID defaults are random, so every scenario supplies ids.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar \
COROUTINES_JAR=.../kotlinx-coroutines-core-jvm-1.9.0.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
