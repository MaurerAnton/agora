# provider-support golden stand

Parity pin for the native port of the shared provider support logic: the HTTP error body
mapping (`ProviderHttpError.kt`), the base-URL version helpers (`BaseUrlResolver.kt`) and the
default endpoint tables (`ProviderDefaults.kt`). Every scenario under `scenarios/` runs through
the **real Kotlin sources compiled from the app tree** and the canonical trace is frozen under
`golden/`; the C++ port (`agora::core::provider_support`, test
`core/tests/provider_support_parity_test.cpp`) must reproduce every trace byte for byte.

These three files are used by every provider's non-200 branch, by the error presentation layer
and by the endpoint configuration path, so one stand pins them once for all of them.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 49 frozen edge cases across three modes |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `ProviderSupportTraceDump.kt` | The stand driver (renders one line per function call) |
| `run_golden.sh` | Compiles the app sources and regenerates or verifies the traces |
| `agora_scenarios provider-support` | Generates the scenarios (deterministic; rerun after editing) |

No extraction step: all three Kotlin files compile standalone (only `GenerationError.kt` and
`Constants.kt` ride along), so the reference is always the real thing.

## Scenario format (`scenarios/*.json`)

```jsonc
{ "name": "...", "mode": "httpError", "statusCode": 429, "rawBody": "{\"error\": ...}" }
{ "name": "...", "mode": "baseUrl", "url": "https://api.example.com/v1" }
{ "name": "...", "mode": "defaults", "embeddingProvider": "mistral",
  "compatibleName": "OpenAI", "openAiMap": { "OpenAI": "https://custom/v1" } }
```

`mode: httpError` calls all three functions and records each result (`parse|`, `http|`,
`structured|` lines): the structured/unstructured classification with `message|detail|reason|
error_description` precedence, the nested-`error` preference, the `code` fallback to the
status code, the `HTTP <status>` fallback for absent/blank bodies and the structured-message
extractor.

`mode: baseUrl` records `hasVersion|`, `withV1|`, `withoutTrailing|` for one URL.

`mode: defaults` records `embedding|`, `compatible|`, `openAiCompatible|` for one provider
name / name / base-url map triple.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
mode|<httpError|baseUrl|defaults>
parse|none
parse|parsed|structured=0/1|code=<esc|->|type=<esc|->|message=<esc>
http|api|code=<esc|->|type=<esc|->|message=<esc>
structured|<esc|->
hasVersion|0/1
withV1|<esc>
withoutTrailing|<esc|->
embedding|<esc>
compatible|0/1
openAiCompatible|<esc>
```

Escaping is the shared `\\` `\p` `\s` `\n` `\r` `\t` set.

## Scope and boundaries

* Transport is out of scope: `HttpClient`, retries and the callers' retry decisions. Only the
  pure decision/parsing halves are pinned here.
* Kotlin semantics pinned explicitly:
  * `JsonNull` never carries a message (a QUOTED `"null"` string does);
  * primitive messages are `trim()`ed; blank after trim → unstructured fallback;
  * `TRAILING_VERSION_SEGMENT` runs with `RegexOption.IGNORE_CASE`, so `/V1` matches while the
    plain `VERSION_SEGMENT` check stays case-sensitive;
  * Java's default `$` anchor accepts a final line terminator — `"…/v1beta\n"` removes up to
    the terminator and keeps it;
  * `embeddingBaseUrl` folds provider names with `String.lowercase()` — ASCII-only here, as
    across the rest of the core port;
  * `isOpenAiCompatibleEmbedding` / the `"OpenAI"` map key are case-sensitive display names;
    an empty configured URL wins over the default (Kotlin `?:` only elides null).

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
core/build/agora_scenarios provider-support core/tools/golden/provider-support                                                          # rewrite scenarios
```

The Kotlin reference is compiled straight from the app tree on every run.
