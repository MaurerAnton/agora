# model-aliases golden stand

Parity pin for the native port of the model-alias inference (`CustomProviderIdentity.kt`'s alias
block). Every scenario under `scenarios/` runs through the **real Kotlin block extracted
verbatim** and the canonical trace is frozen under `golden/`; the C++ port
(`agora::core::aliases::infer_model_alias`, test `core/tests/model_aliases_parity_test.cpp`)
must reproduce every trace byte for byte.

## How the stand works

The identity file also owns provider-id normalization and the conversation display helpers. The
alias block (the regexes, the display-name tables, `inferModelAlias` and its Gemini / Claude /
Nova branches, `formatAliasToken` and `isValidSnapshotDate`) is one self-contained top-level
region with no Android or viewmodel dependencies, so ``agora_extract aliases`` copies it verbatim —
only `private` is widened to `internal`. The driver calls `inferModelAlias` directly, so there is
**no transcription**.

## Scenario format and trace

`{name, mode: "alias", modelName}`

```
name|<scenario name>
mode|alias
alias|<the inferred display alias>
```

## Pinned semantics

* **Path tail and variant suffix**: the text after the last `/` (the whole id when that is blank)
  with a trailing `:batch`/`:free` removed (case-insensitive); an emptied tail falls back to the
  path tail.
* **Gemini**: only a `-preview` slug becomes the alias — `gemini-2.5-pro-preview` →
  `Gemini 2.5 Pro`, while `gemini-2.5-pro` falls through to the token humanizer.
* **Claude**, family-first and version-first: a valid eight-digit snapshot is dropped
  (`claude-sonnet-4-5-20250929` → `Claude Sonnet 4.5`, `claude-3-5-sonnet-20241022` →
  `Claude 3.5 Sonnet`), but an **invalid** date rejects the whole pattern
  (`LocalDate.parse`): 20250230, 20250001 and 20230229 fall back to `Claude Sonnet 4 5 20250230`
  while 20240229 (a real leap day) is accepted; a short tail like `-2025` is not a snapshot.
* **Nova**: `nova-pro-v1:0` → `Nova Pro Pro` (the greedy middle takes the last `-v` marker).
* **Token formatting**: the exact table (`a3b` → `A3B`), the token table (`gpt` → `GPT`), the
  numeric-suffix rule scanned in table insertion order (`qwen3` → `Qwen 3` **with a space**,
  `llama3` → `Llama3`, `gpt4` → `GPT4`), the version tokens (`v2`, `r1-preview` → `R1 Preview`
  after splitting), the `o` series, and the size tokens where the unit is upper-cased
  (`120b` → `120B`, `1.5m` → `1.5M`).
* Case folding and the per-character `titlecase` are ASCII-only, as across the rest of the port.

## Scope

Only `inferModelAlias`. The id normalization (`CustomProviderIdentityPolicy.normalize`, legacy
UUID ids) and the conversation/provider display helpers stay in Kotlin — see `core/README.md`.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is extracted and compiled from the app tree on every run; the extractor fails
loudly if the alias block moves.