# model-thinking-resolver golden stand

Pins `data/ModelThinkingCapabilityResolver.kt` — the bridge from a persisted selected-model string
to the thinking options a provider endpoint actually accepts.

## Verbatim sources

Compiled from the app tree without edits:

- `data/ModelThinkingCapabilityResolver.kt`
- `model/ModelId.kt`
- `data/CustomProviderConfig.kt` (including `CustomEndpointProtocol` and its serializer)
- `model/ModelThinkingCapability.kt`
- `model/ModelThinkingCapabilities.kt`
- `model/ModelThinkingCapabilityDefaults.kt`
- `model/ThinkingControl.kt` (`ThinkingLevels`)
- `util/Constants.kt`

`CustomProviderIdentity.kt` cannot compile host-side: the same file contains Room/model-migration
code and UI imports. `CustomProviderConfig.providerId` only needs
`CustomProviderIdentityPolicy.isStableId`, so `extract_identity_policy.py` regenerates that object
slice from the real file on every run (pattern, compiled regex and function body copied verbatim).
The full identity policy is pinned by the separate `provider-identity` stand.

## What is frozen

36 traces cover:

- every `ModelId.parse` heuristic: `gpt-`, `o1`/`o3`/`o4` (including a no-dash prefix), `claude-`,
  `deepseek`, `qwen`, `groq`, `models/`/`gemini`, unknown and case-sensitive uppercase misses;
- prefixed ids: empty provider, empty model, first-colon split, and colons inside the model name;
- custom-provider matching by exact display name, stable id, uppercase stable id, whitespace-padded
  stable id, and legacy name; first-match order and case-sensitive name misses;
- every custom protocol: OpenAI, Google, Anthropic, `gemini`/`claude` aliases, case/whitespace
  normalization, blank and unknown wire values;
- the resulting `ModelThinkingCapability`: all fields plus the derived `supportsEffort` and
  `lowestEffort`.

## Run

```bash
export JAVA_HOME=...
export PATH="$JAVA_HOME/bin:$PATH"
export KOTLINC=/path/to/kotlinc
export KOTLINX_JARS=/path/to/kotlinx-serialization-core-jvm.jar:/path/to/kotlinx-serialization-json-jvm.jar
./run_golden.sh            # verify scenarios against golden/
./run_golden.sh --freeze   # regenerate after changing scenarios or the Kotlin source
```

The C++ side is checked by `core/tests/model_thinking_resolver_parity_test.cpp` (test
`model_thinking_resolver_golden_parity`), which iterates the same scenarios and compares the full
trace byte for byte.
