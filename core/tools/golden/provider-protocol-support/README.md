# provider-protocol-support golden stand

Pins the provider-protocol helpers in `data/CustomProviderConfig.kt`: protocol detection, prompt
cache switches, Anthropic cache TTL and Responses API availability.

Compiled verbatim from the app tree:

- `data/CustomProviderConfig.kt`
- `util/Constants.kt`

`CustomProviderIdentity.kt` contains Room/model-migration code and cannot compile host-side.
`CustomProviderConfig.providerId` only needs `CustomProviderIdentityPolicy.isStableId`, so the
sibling `model-thinking-resolver` stand's `agora_extract identity-policy` regenerates that object slice
from the real file on every run. The full identity policy is pinned separately by the
`provider-identity` stand.

## What is frozen

28 traces cover:

- built-in `OpenAI` / `Anthropic` defaults and false switches;
- custom providers matched by display name, stable id and legacy name, including case sensitivity;
- every protocol: OpenAI, Anthropic, Google, unknown;
- any-match protocol predicates vs first-match cache/TTL/Responses decisions;
- `responsesApiEnabled`, `anthropicCacheEnabled` and custom `anthropicCacheTtl`;
- the fallback `true`/`"1h"` values when no custom provider matches;
- built-in names shadowing a same-named custom provider in the `when` branches while still using
  the first custom match for cache/TTL/Responses.

## Run

```bash
export JAVA_HOME=...
export PATH="$JAVA_HOME/bin:$PATH"
export KOTLINC=/path/to/kotlinc
export KOTLINX_JARS=/path/to/kotlinx-serialization-core-jvm.jar:/path/to/kotlinx-serialization-json-jvm.jar
./run_golden.sh            # verify against golden/
./run_golden.sh --freeze   # regenerate after changing scenarios or the Kotlin source
```

C++ parity: `core/tests/provider_protocol_support_parity_test.cpp`
(`provider_protocol_support_golden_parity`).
