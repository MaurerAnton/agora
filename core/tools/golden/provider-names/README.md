# provider-names golden stand

Parity pin for the native port of the custom provider name policy (`CustomProviderNamePolicy.kt`).
Every scenario under `scenarios/` runs through the **real Kotlin sources compiled verbatim** and
the canonical trace is frozen under `golden/`; the C++ port
(`agora::core::provider_support::is_allowed_custom_provider_name` / `has_custom_provider_name_conflict`
/ `sanitize_custom_provider_names`, test `core/tests/provider_names_parity_test.cpp`) must
reproduce every trace byte for byte.

## How the stand works

`CustomProviderNamePolicy.kt` and `CustomProviderConfig.kt` compile **verbatim** from the app
tree (they only need `Constants.kt`), so the driver calls them directly — no transcription. The
config model reaches its identity through
`CustomProviderIdentityPolicy::isStableId`, and that policy's file also owns conversation-model
helpers; `extract_identity.py` therefore copies the **stable-id pattern and the check verbatim**
into a stand-alone object, keeping the real regex in the loop.

## Scenario format and trace

`{name, mode, providerName, existingNames?, currentName?, configs?}` with
`mode: allowed | conflict | sanitize`:

```
name|<scenario name>
mode|<allowed|conflict|sanitize>
allowed|0/1                      (isAllowed)
conflict|0/1                     (hasConflict with existingNames/currentName)
accepted|<n> / accept|<i>|<esc>  (sanitize result, original names preserved)
rejected|<n> / reject|<i>|<esc>
```

## Pinned semantics

* The reserved set is the ten built-in display names (`Google`, `OpenAI`, `Anthropic`,
  `DeepSeek`, `Qwen`, `Groq`, `Ollama`, `Open Router`, `OpenCode Go`, `Local`); the comparison is
  **trimmed and case-insensitive**, so `"  Anthropic "` and `"GOOGLE"` are rejected while
  `"Googler"` and `"Deep  Seek"` are allowed.
* `hasConflict` returns true for a disallowed name regardless of the existing set; otherwise it
  compares trimmed, case-insensitively against the existing names — and the entry matching
  `currentName` is exempt via an **exact, case-sensitive** equality (pinned: `existing=["Box"]`,
  `currentName="box"` still conflicts).
* `sanitize` keeps the first of each name and quarantines the rest **without rewriting** them, so
  the rejected list carries the original (untrimmed) strings — a collision such as `Local` never
  mutates the name-keyed settings.
* The port folds case ASCII-only (`String.equals(ignoreCase = true)` in Kotlin also folds Unicode;
  the corpus pins the ASCII cases and the boundary is documented).

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly if
the stable-id pattern moves.