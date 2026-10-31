# export-extra-settings golden stand

Parity pin for `data/ExportExtraSettings.kt::restoreLegacyFromJsonObject`, the reader for
`extra_settings.json` inside backups whose manifest version is below 4.

`ExportExtraSettings.kt`, `SettingsContracts.kt` (`ConversationSettings`), `ThinkingControl.kt`,
`OpenAiServiceTiers.kt` and their shared settings models compile **verbatim from the app tree**.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 164 legacy-object cases with the replace/secrets/allowed-id arguments and seeded alias/model flows |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `ExportExtraSettingsTraceDump.kt` | Driver: runs the reader and prints the recorded call log |
| `platform/` | DataStore preferences keys, crypto/debug shims, recording `SettingsManager` double |
| `agora_scenarios export-extra-settings <stand>` | Deterministic scenario generator |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **Call log and order**: every `SettingsManager.save*` call is recorded with its exact argument,
  in the order the reader performs it. The 38 scalar fields are covered both present and absent.
* **Primitive coercions**: `JsonPrimitive.int` and `.float` accept the same trimmed spellings as
  the reference, including decimal exponents (`3e2`, `10e-1`), leading zeros, Java hexadecimal
  floats with a mandatory `p` exponent, `f/F/d/D` suffixes, `NaN`/`Infinity` spellings and
  overflow to infinity. `Boolean` accepts case-insensitive `true`/`false` and rejects every other
  content. `contentOrNull` stringifies numbers/booleans and drops `null`.
* **Conversation settings**: only ids in `allowedConversationIds` are read; all-null records are
  not saved; the legacy `budget:<Int>` form fills the independent budget fields only when the
  explicit keys are absent; `thinkingLevel` and `openAiServiceTier` are normalised through the
  shipped policies; the remaining fields are copied with their nullable shape intact.
* **Aliases and models**: `modelAliases` replace vs. merge keeps `LinkedHashMap` positions, drops
  null/empty primitive values and rejects non-primitive values; `customModels` is a
  `LinkedHashSet`, is written even when the array is empty, and merges or replaces the seeded set.
* **Secrets**: `proxyPassword` is ignored without `allowSecrets` and when empty; `proxyBypass` is
  ignored only when empty (a single space is written).
* **Failure shape**: a mistyped value throws the same category
  (`IllegalArgumentException`/`NumberFormatException`/`IllegalStateException`) and writes made
  before the throw remain in the trace. Messages are not part of the contract.

## Boundaries

* The real `SettingsManager` is a DataStore-backed Android class. `platform/SettingsManager.kt`
  records the save arguments and seeds the two flows the reader consumes. It deliberately does
  **not** reproduce storage-side coercions (`saveWebSearchNumResults` clamping, blank filtering,
  conversation-tier re-normalisation, …): those belong to `SettingsManager`, not to this reader,
  and the trace pins exactly what the reader hands over.
* Exception messages are kotlinx/JVM diagnostics and are not pinned; only the category is.
* Boolean case folding is ASCII here, while Kotlin's ignore-case comparison is Unicode-aware.
  Every real spelling is ASCII; an exotic Unicode lookalike could only fail to match.
* System prompts are restored from `system_prompts.json`, not from this object, matching the
  comment in the Kotlin source.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
COROUTINES_JAR=/home/user/agora-toolchain/kotlinc-2.3.21/lib/kotlinx-coroutines-core-jvm.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
