# shell-crypto golden stand

Parity pin for the native port of `util/ShellCrypto.kt` (X25519 key handling, ECDH + HKDF,
AES-256-GCM, HMAC signing, response auth, nonce and base64url helpers). Every scenario under
`scenarios/` runs through the **real Kotlin object compiled verbatim from the app tree** and the
canonical trace is frozen under `golden/`; the C++ port
(`agora::core::shell_crypto`, test `core/tests/shell_crypto_parity_test.cpp`) must reproduce
every trace byte for byte **after the same non-determinism mask** is applied to both sides.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 27 frozen scenarios |
| `golden/*.trace` | Frozen canonical output, one per scenario (already masked) |
| `ShellCryptoTraceDump.kt` | The stand driver (exercises every public function) |
| `agora_scenarios shell-crypto` | Generates the scenarios (deterministic; rerun after editing) |
| `platform/Base64.java` | The substituted `android.util.Base64` surface (vendored) |
| `agora_scenarios mask-trace <file>` | Masks non-deterministic trace fields **with their length** on both sides |
| `run_golden.sh` | Extracts nothing, compiles and regenerates or verifies the traces |

## What is pinned

* **Key handling** — `generate_ephemeral_keypair` emits the raw 32-byte public key (base64url)
  and the DER PKCS#8 private key (**standard** base64, no padding, exactly what
  `java.security.PrivateKey.encoded` + `Base64.getEncoder().withoutPadding()` produce);
  `encode_public_key`/`decode_public_key` round-trip the raw key and reject the wrong length
  with `invalid X25519 public key length: <n>` (the size is part of the message) and a bad
  alphabet with the java.util description `Illegal base64 character <hex>`.
* **ECDH + HKDF** — `derive_aes_key` pins the derived key for a fixed key pair: the raw
  Diffie-Hellman output is Extracted with an all-zero 32-byte salt and then Expanded with the
  `conch-agora-v2` info (RFC 5869 order, one block).
* **AES-256-GCM** — round-trips over empty, short and 10 KB plaintexts (ciphertext masked); **27**
  decrypts a ciphertext produced by an independent implementation (Python's `cryptography`) with
  the right key, so a self-consistent but wrong port of the cipher cannot pass; the failure modes
  are pinned at their exact boundaries:
  * raw < 13 bytes → `ciphertext too short: <n> bytes` (the `+1` after the nonce),
  * body shorter than the 16-byte tag (1 … 15 bytes) → `javax.crypto.ShortBufferException: Output buffer invalid`,
  * body ≥ 16 with a wrong key → `Tag mismatch!`,
  * an alphabet violation → `Illegal base64 character <hex>`.
* **HMAC/SHA** — `sign` pins the canonical `timestamp|method|path|body_sha256|nonce|client_pub_key`
  message (the api key is the HMAC **key**, never part of the message) and `sha256_hex` pins the
  well-known digest of `hello world`.
* **Response auth** — `response_signature` pins that the session key is a PRK that is only
  **Expanded** (no Extract) with `conch-response-auth-v1`, over the payload
  `conch-response-v1|<status>|<sha256hex(body)>`; `verify_response_signature` pins the valid case
  (placeholder = recompute) and a 64-zero wrong signature.
* **Nonce / base64url** — `generate_nonce` draws **12** bytes (16 base64url chars, masked);
  encode/decode cover ASCII, padded input and `+/`-only bytes mapped to `-_`.

## Scenario format (`scenarios/*.json`)

```json
{ "op": "decrypt", "key": "<base64url 32 bytes>", "ciphertext": "..." }
```

`op` selects the ShellCrypto function; the remaining fields are the driver-level inputs (base64url
decoding of `key`/`session_key`/`private_key_pkcs8` happens **in the driver**, matching
`JvmBase64.getUrlDecoder()`). The literal `PLACEHOLDER_WILL_BE_FILLED_BY_DRIVER` makes the driver
compute the expected value itself: for `decrypt` it always round-trips the literal `"test"`.

## Trace format (`golden/*.trace`)

One compact JSON object per line, keys in driver order, e.g.
`{"derived_key":"…"}` or `{"error":"Tag mismatch!"}`; escaping is the driver's
(`\` `"` `\n` `\r` `\t` only). Every trace ends with `\n`.

**Masking**: `run_golden.sh` runs `agora_scenarios mask-trace` over the fresh traces *and* the frozen golden
traces before diffing (and over the golden on `--freeze`). `nonce`, `ciphertext`, `public_key_raw`
and `private_key_raw` are replaced with **length-carrying** placeholders — `<nonce:16>`,
`<ciphertext:55>`, `<public_key_raw:43>`, `<private_key_raw:64>` — while everything else stays
byte-exact. The length is what keeps the *sizes* pinned through the mask: `generateNonce` drawing
12 bytes instead of 16, a raw key that is not 32 bytes, a DER private key of the wrong length or
a nonce+tag that is not 12+16 bytes all still fail the trace. The C++ test applies the same four
rules (alphabet and minimum length included — the private key uses the standard alphabet, which
is why the mask distinguishes them).

## Scope and boundaries

* `ShellCrypto.kt` compiles verbatim. The only substituted platform type is
  `android.util.Base64` (`platform/Base64.java`), which delegates to `java.util.Base64`.
  **Boundary**: real Android rejects malformed input with android's own descriptions, the vendored
  stand type raises `java.util.Base64`'s `Illegal base64 character <hex>` — the traces pin the
  stand type's text, and the C++ port reproduces it (`base64_decode` in `crypto_primitives.cpp`).
* The C++ side carries **no third-party dependency** (`core/README.md` rule): SHA-256 is the
  in-repo `sha256.hpp`, and HMAC, HKDF, base64, X25519, PKCS#8 and AES-256-GCM are implemented
  from their RFC/FIPS specifications in `crypto_primitives.cpp`. Besides the parity replay, their
  known-answer vectors (generated from Python's independent implementation) are pinned by
  `core/tests/crypto_primitives_test.cpp`.
* `X25519_SPKI_PREFIX` (12 bytes) is **hardcoded in the driver**: it is `private` in Kotlin and is
  derived lazily from a real keypair, so the stand cannot read it. The port never materializes
  SPKI: `encodePublicKey` only ever emits the trailing raw key and `decodePublicKey` only ever
  validates its length, so both reduce to raw-key handling.
* The driver's `encode_public_key`/`derive_aes_key`/`sign`/`sha256_hex`/`response_signature`/
  `verify_response_signature`/`base64_*` branches have no catch — a Kotlin exception would abort
  the run instead of freezing a trace; the only error paths exercised are the ones pinned above.
* Non-deterministic outputs (key generation, nonce, GCM ciphertext) never reach the diff
  unmasked.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
core/build/agora_scenarios shell-crypto core/tools/golden/shell-crypto                # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (git-ignored scratch) before each run, so runs are
idempotent. The scenario generator needs `cryptography` (only for the fixed AES-GCM ciphertext in
`21-decrypt_auth_failure`).
