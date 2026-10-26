# conch-handshake golden stand

Parity pin for the native port of `util/ConchHandshake.kt` (the handshake verification the Conch
shell client performs against the server). Every scenario under `scenarios/` runs through the
**real Kotlin data class compiled verbatim from the app tree** and the canonical trace is frozen
under `golden/`; the C++ port (`agora::core::conch::Handshake`,
test `core/tests/conch_handshake_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 11 frozen scenarios |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `ConchHandshakeTraceDump.kt` | The stand driver (decodes the handshake JSON, calls `verify`) |
| `gen_scenarios.py` | Generates the scenarios (HMACs computed with Python's `hmac`) |
| `platform/Charsets.kt` | The substituted `java.nio.charset.Charsets` surface (stand-local) |
| `run_golden.sh` | Compiles the stand-local platform type and regenerates or verifies |

## What is pinned

`verify(apiKey, expectedChallenge)` is a conjunction; the traces pin the single `true` case and
one `false` per failed conjunct:

* **01-valid_full** — all four required features, both HMACs computed over
  `nonce|public_key` and `nonce|conch-handshake-v2|public_key|challenge|features`, matching
  challenge → `true`.
* **02/03** — an empty `expectedChallenge` is rejected and a challenge that does not equal the
  handshake's own `challenge` is rejected.
* **04** — an empty nonce fails before any HMAC runs.
* **05/09/10** — the main signature wrong (64 zeros), **short** (`"abc"`: `signature.length != 64`
  short-circuits before HMAC) and an empty `features_signature`.
* **06** — features signature mismatch while the main signature stays valid.
* **07** — two of the four required features missing from the comma-separated list.
* **08** — an *extra* feature **besides** the required four still verifies (`containsAll`, not
  equality) — note the `features_signature` covers the extended list exactly as the server signs
  it; a tampered list is already pinned separately by 06.
* **11** — signatures recomputed with a different api key fail against the expected key.

## Scenario format (`scenarios/*.json`)

```json
{
  "op": "verify",
  "api_key": "test-api-key-123",
  "expected_challenge": "challenge456",
  "handshake": {
    "public_key": "...", "nonce": "...", "signature": "...",
    "features": "hkdf-extract-v2,response-auth-v1,sse-sequence-v1,rejection-auth-v1",
    "challenge": "...", "features_signature": "..."
  }
}
```

## Trace format (`golden/*.trace`)

`{"valid":true}` or `{"valid":false}` — the kotlinx `buildJsonObject { put("valid", …) }`
output, one line per scenario, `\n` terminated.

## Scope and boundaries

* `ConchHandshake.kt` compiles verbatim; the only substituted platform type is
  `java.nio.charset.Charsets` (`platform/Charsets.kt`, compiled with **kotlinc**, not javac,
  because the stand-local object extends `StandardCharsets`). It exposes exactly the `UTF_8` and
  `US_ASCII` constants the file touches.
* The driver re-serializes the parsed `handshake` object **compactly** (kotlinx
  `JsonObject.toString()`, member order preserved) before handing it to `ConchHandshake.decode`;
  the C++ test mirrors that serialization, so `decode` is exercised on the same bytes Kotlin
  feeds it.
* A handshake missing `public_key`/`nonce`/`signature` makes kotlinx throw during decode, which
  aborts the driver instead of freezing a trace; the C++ port substitutes `""` for a missing
  member, which fails `verify` the same way the pinned empty-field scenarios (04/10) do.
* The comparison inside `verifyPayload` is `MessageDigest.isEqual` (constant time) on the ASCII
  bytes of the hex strings; the C++ port XOR-folds instead — same result, both length-gated by
  `signature.length != 64`.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (git-ignored scratch) before each run, so runs are
idempotent. As with the other stands, kotlinc needs an explicit `JAVA_HOME`.
