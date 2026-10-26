# conch-network golden stand

Parity pin for the shell transport policy in `util/ConchNetwork.kt` and the bounded HTTP reads in
`api/BoundedWireRead.kt`.

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `app/.../util/ConchNetwork.kt` | the module under test: client policy, GET/POST request construction, per-call timeout, response classification and body limit selection |
| `app/.../api/BoundedWireRead.kt` | the text/line transport bounds and their exact failure messages |

## Substituted platform halves

| Substitute | Contract |
| --- | --- |
| `platform/okhttp3/OkHttp.kt` | the subset of OkHttp 5 that `ConchNetwork.kt` touches: `Request.Builder.header` replaces case-insensitively, `Headers.get` returns the last matching value, `Response.isSuccessful` is 200..299, `Call.timeout()` records the per-call timeout. It records the request instead of opening a socket. |
| `platform/okio/Okio.kt` | `BufferedSource` over scripted bytes, including `request(n)`, `readUtf8`, `exhausted`, `readUtf8LineStrict` and `buffer.size`. `ByteSource.declaredSize` may exceed the materialised bytes so an over-limit rejection is pinned without allocating 40 MiB. |
| `platform/HttpClient.kt` | `com.newoether.agora.api.HttpClient.client` — the provider-wide base client. The real object owns provider timeouts, proxy selection and diagnostics; only its use as the base for `ConchNetwork`'s builder matters here. |

Sockets, TLS, redirects, proxy selection, connection reuse and the actual OkHttp call stay on the
platform side. The core port owns the policy and consumes a `Transport`/`WireSource`; the app
adapter fills those with OkHttp/okio. `readBoundedWireText` returns the UTF-8 bytes; the adapter
decodes them like okio's `readUtf8` (the traces use valid UTF-8 bodies).

## What the traces pin

- the client policy: `retryOnConnectionFailure=false`, `followRedirects=false`,
  `followSslRedirects=false`, pool `5` / `45s`;
- GET: method, URL, caller headers after case-insensitive replacement, `timeoutMillis=30000`,
  no body/content type;
- POST: `application/json` request body, default `timeoutMillis=130000`, explicit timeouts, the
  `require(timeout > 0)` rejection before any call, and caller headers overriding the body media
  type header;
- response: 200..299 success, signature lookup case-insensitively with the last value winning,
  missing signature as `""`, and `Response.use` closing on both success and read failure;
- body limits: 64 KiB for GET success/failure and for any failure response, 40 MiB for POST
  success; at-limit acceptance and limit+1 rejection for both; the exact
  `Response exceeds the N-byte transport limit` message;
- `readBoundedWireText`: positive-limit requirement, at-limit acceptance, over-limit rejection,
  UTF-8 byte counting, `declaredSize` larger than the materialised body;
- `readBoundedWireLine`: clean-EOF `null`, LF and CRLF stripping, `Event exceeds the N-byte
  transport limit`, `Incomplete encrypted event line`, and the positive-limit requirement.

## Usage

```sh
KOTLINX_JARS=... ./run_golden.sh            # verify against the frozen traces
KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate golden/
```
