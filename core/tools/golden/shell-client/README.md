# shell-client

Golden stand for `util/ShellClient.kt` — the Conch HTTP client that `ConchBackend` and the
shell tool providers sit on: the security handshake, the encrypted-request assembly, every
file/jobs endpoint's request payload and response coercion, and the `describeConch*Failure`
message table.

## What compiles verbatim

| Source | Notes |
| --- | --- |
| `app/src/main/java/com/newoether/agora/util/ShellClient.kt` | **verbatim** — no edits |
| `app/src/main/java/com/newoether/agora/util/ShellCrypto.kt` | **verbatim** (real crypto: X25519, HKDF, AES-256-GCM, HMAC) |
| `app/src/main/java/com/newoether/agora/util/ConchHandshake.kt` | **verbatim** (`@Serializable`, compiled with the kotlinx plugin) |

## Platform doubles (`platform/`)

1. **`ConchNetwork.kt`** — the OkHttp GET/POST surface with a recorded request log. Responses
   come from the scenario script:
   - transport failures are thrown as the mapped exception type (`UnknownHost`, `Connect`,
     `SocketTimeout`, `InterruptedIO`, `SSL`, plain) so `describeConch*Failure` runs for real;
   - a `dynamicHandshake` body is built by the driver from the request's `?challenge=` value
     (the HMAC covers a random per-run challenge — the trace masks the challenge, the outcome
     is deterministic);
   - on a successful **encrypted** call (`X-Encryption` header, 2xx) the double seals the
     scripted plaintext with the *exact* session key `ShellClient` derived — X25519 is
     symmetric, so it recomputes `serverPrivate × clientPublic` from the fixed stand server
     private key and the request's `X-Client-Public-Key` — and signs it with the real
     `ShellCrypto.responseSignature`. Sealed responses are signed by default (`"signature":
     "invalid"` opts out to pin the auth-failure message); non-2xx and plain bodies pass
     through verbatim so HTTP-error text stays deterministic.
2. **`DebugLog.kt`** — silent no-op (the real object wraps `android.util.Log`).
3. **`Base64.java`** — vendored `android.util.Base64`, identical to the shell-crypto stand's.

Fixed stand identity: one X25519 keypair is embedded as constants (`SERVER_PUBLIC_KEY` in the
driver, the matching PKCS#8 in the double), so handshake `public_key`, `decodePublicKey` and
the response-sealing key derivation are all pinned without per-run drift.

## Determinism strategy

Random inputs (challenge, nonce, timestamp, ephemeral keypair, ciphertext) are either masked
in the trace (`<challenge>`, `<nonce>`, `<signature>`, `<timestamp>`, `<client-public-key>`,
`<encrypted>`) or never reach the outcome, so every frozen byte is stable across runs while
the request *shape* — URL paths, header names and order, `X-Encryption: v2`, typed JSON
payloads for the plain-text endpoints — stays fully pinned.

## Scenario ops (83 scenarios)

| Op | Pins |
| --- | --- |
| `fetch_public_key` | success, blank-api-key message, wrong-HMAC/missing-features/challenge-mismatch handshake failures, HTTP failure text (+240-char body take + `empty response`), all six `describeConchConnectionFailure` branches, silent cached-key decode fallback, `invalid X25519 public key length` after a valid handshake |
| `prepare_request` | plain branch (single `Content-Type` header + verbatim command JSON incl. workdir omission), encrypted branch (full header order with masked values, `X-Encryption: v2`), `security handshake is required` gate, Unicode command |
| `decrypt_sse` | `No session key`, sealed roundtrip, `ciphertext too short: N bytes` raw throw |
| `file_read` | the whole response-coercion matrix (error field incl. `{"error":null}` → `"null"`, `lines`/`size` string-vs-number parsing, `truncated` strict-bool + fallback, offset/limit clamping and the typed request payload), HTTP errors on both plain and encrypted paths, all six `describeConchRequestFailure` branches with the `/file/read request` operation text, encrypted auth-failure / decrypt-failure / success |
| `file_write` / `file_edit` | request payload typing (`replace_all` boolean), error string vs success, `replacements` string coercion, missing-field defaults, `{"error":null}` edge, encrypted success |
| `file_glob` / `file_grep` | `path`/`depth`/`glob` omission vs typed inclusion, `truncated` fallbacks, `Result.failure` message, non-array `files`, match-field coercions (`line:"abc"` → 0) |
| `file_image` | data/mimeType/size coercion, error branch, missing fields |
| `job_*` | `/jobs/{start,list,get,stop,ack}` URLs, payload builders (workdir omission, `job_id`), `callTimeoutMillis` passthrough (null vs number), passthrough vs HTTP error, encrypted success |

## Running

```sh
JAVA_HOME=/path/to/jdk17 \
KOTLINC=/path/to/kotlinc/bin/kotlinc \
KOTLINX_JARS=/path/kotlinx-serialization-core-jvm.jar:/path/kotlinx-serialization-json-jvm.jar \
./run_golden.sh            # verify against golden/
./run_golden.sh --freeze   # regenerate golden/ from the Kotlin reference
```

The kotlinx-coroutines jar is located next to the serialization jars (override with
`KOTLINX_COROUTINES_JAR`); the serialization compiler plugin is required for
`ConchHandshake.@Serializable`; `platform/Base64.java` is compiled with `javac` first.
