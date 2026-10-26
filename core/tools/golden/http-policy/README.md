# http-policy golden stand

Parity pin for the transport-neutral policy in `api/HttpClient.kt`: host locality, the cleartext
credential guard, proxy sanitisation, proxy bypass/CIDR matching, proxy authorization and the
base client timeouts/redirect policy. The actual OkHttp client (sockets, TLS, calls, streaming,
diagnostics) stays on the platform side.

`HttpClient.kt` is a 608-line OkHttp/Android object. `extract_policy.py` copies the pure
declarations out of it verbatim, changes only their visibility (`private` -> `internal`) and
dedents them out of `object HttpClient`. The derived wrappers at the bottom parameterise the
stored `proxyConfig` field; the extractor asserts the source snippets they rely on are still
present, so a shape change in `HttpClient.kt` fails the stand instead of drifting silently.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 158 frozen scenarios |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `HttpPolicyTraceDump.kt` | The stand driver |
| `extract_policy.py` | Lifts the policy declarations out of `HttpClient.kt` |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |

## What is pinned

- **Host locality** (`isLocalHost`): loopback, RFC-1918, link-local, CGNAT `100.64/10`, Tailscale
  MagicDNS (`*.ts.net`) and the `fd7a:115c:a1e0` ULA prefix; `.local`/`.lan`/`.home`/`.internal`
  suffixes; bare names without a dot; bracket trimming; the IPv4 octet scanner (leading zeros,
  `+`/`-`, trailing empty parts, BMP Unicode decimal digits via the JDK 17 `Nd` table, but not
  astral digits because Kotlin iterates UTF-16 chars); public FQDNs/IPs and `0.0.0.0` as
  non-local.
- **Cleartext credential guard**: only `http://` (case-insensitive) is checked; the host comes
  from `java.net.URI`; the four credential header names are matched case-insensitively; local
  hosts, `https`, other schemes, non-credential headers and empty headers pass; public hosts,
  registry authorities (`foo_bar`), userinfo URLs, malformed URLs and trailing-dot hosts throw the
  exact `IOException` message.
- **Proxy sanitisation**: `host.isNotBlank()` (Kotlin whitespace incl. NBSP, not zero-width
  space) and `port in 1..65535`; the host is kept untrimmed.
- **Proxy bypass**: exact host, `*.suffix` (apex and subdomains), IPv4 CIDR (`/0`, `/32`, invalid
  bits/shape), whitespace-trimmed entries, bracket trimming on the host (not the entry), and the
  Kotlin `toIntOrNull` quirks (`+16`, `016`, `-0`).
- **Proxy decision**: direct when there is no sanitized config or a bypass matches; otherwise the
  configured HTTP/SOCKS type with the unresolved host/port.
- **Proxy authorization**: HTTP only, non-blank username (Kotlin whitespace), no existing
  `Proxy-Authorization` header, and `Basic` + Base64 of the ISO-8859-1 `username:password` bytes
  (unmappable code points become `?`).
- **Base client policy**: connect 30s, read 5min, write 30s, OkHttp 5 retry/redirect defaults.

## Scenario format

```json
{ "op": "guardCleartextCredentials", "url": "http://api.example.com/v1",
  "headers": [["Authorization", "Bearer secret"]] }
```

The driver echoes the inputs and appends `outcome`. Proxy scenarios use:

```json
{ "op": "resolveProxy", "host": "api.example.com",
  "config": { "type": "HTTP", "host": "proxy.test", "port": 8080,
              "username": "", "password": "", "bypass": [] } }
```

`config` may be `null`/absent. `resolveProxy` and `proxyAuthorization` run the same
`sanitizedProxyConfig` step as `setProxy`; the echoed `config` is the raw scenario value.

## Trace format

One compact `JsonObject.toString()` line per scenario, e.g.

```json
{"op":"isLocalHost","host":"localhost","outcome":{"local":true}}
```

The C++ side builds the same object with `json::to_kotlin_string` (insertion order preserved) and
the test compares the bytes, including the trailing newline.

## Scope and boundaries

- `extract_policy.py` copies `CREDENTIAL_HEADERS`, `isLocalHost`, `guardCleartextCredentials`,
  `ProxyType`, `ProxyConfig`, `isProxyBypassed`, `ipv4ToLong` and `ipv4InCidr` from
  `HttpClient.kt`, changing only visibility and indentation. `sanitizedProxyConfig`,
  `resolveProxy`, `proxyAuthorizationHeader` and `clientPolicy` are small derived wrappers; the
  extractor checks the exact source lines they mirror.
- **Platform substitutions**: `okhttp3.Credentials.basic` is replaced by `java.util.Base64` over
  `Charsets.ISO_8859_1`, which is what OkHttp 5 does; the base client policy constants are
  derived from the `OkHttpClient.Builder` calls. `java.net.URI` is the JVM parser in the stand
  and the existing `core/java_uri` port in C++.
- Kotlin `String.lowercase()` is Unicode-aware; the C++ port folds ASCII only, the established
  core convention. Hosts and header names in the pinned corpus are ASCII; the cleartext guard
  treats a non-ASCII uppercase suffix the same way only when the case fold is ASCII.
- The `setProxy` side effect on `java.net.Authenticator` (SOCKS proxy auth) and OkHttp's actual
  proxy selection/connection are platform concerns; the core port owns the decisions pinned here.
- The C++ port uses the existing `core/java_uri` (JDK 26 semantics) and `core/crypto_primitives`
  Base64; no third-party dependency is added.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc \
KOTLINX_JARS=core.jar:json.jar ./run_golden.sh            # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc \
KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze   # regenerate
python3 gen_scenarios.py                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (git-ignored scratch, including the extracted
`HttpPolicy.kt`) before each run.
