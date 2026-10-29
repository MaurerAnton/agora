# zip-reader golden stand

Parity pin for the ZIP reader the app needs to read a vendor export archive: the
`java.util.zip.ZipInputStream` surface (local file headers, STORED and DEFLATED entries, data
descriptors) and the JDK's failure messages.

The reference is the JDK itself — the driver builds archives with `ZipOutputStream` and reads them
back with `ZipInputStream`, so the traces are the JVM's behaviour, byte for byte. No app source is
involved: this is the platform archive layer the vendor importers sit on.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 39 cases: built archives, raw byte archives, corruption paths, deterministic fuzz |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `fixtures/*.zip` | The archives the driver built (committed so the C++ port reads the same bytes) |
| `ZipReaderTraceDump.kt` | Driver: builds/reads the archive and renders the entries |
| `agora_scenarios zip-reader` | Deterministic scenario generator (raw archives are assembled byte by byte) |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **The entry list**: name, method, uncompressed size, compressed size, CRC and content for every
  entry, in local-header order.
* **Both compression methods**: STORED and DEFLATED — the latter through the JDK's own deflater,
  so the port's from-scratch RFC 1951 decoder has to reproduce the *same bytes* for stored blocks,
  fixed Huffman blocks, dynamic Huffman blocks, long back-references across the 32 KiB window
  (the 160 KB repetitive case) and incompressible payloads (stored blocks inside a deflate stream).
* **Data descriptors**: the archives the JDK writes carry the sizes and the CRC *after* the
  payload (general purpose flag bit 3); the trace shows the descriptor's values, and the port has to
  find the descriptor behind the inflated stream.
* **The extra field** is skipped, and UTF-8 entry names survive unchanged.
* **The JDK's failure behaviour**:
  * a foreign signature (or a truncated tail) ends the archive **silently** — no entries, no error;
  * a truncated header is `ZipException("unexpected EOF")`;
  * an unsupported method is `ZipException("invalid compression method")`;
  * a STORED entry with a data descriptor is
    `ZipException("only DEFLATED entries can have EXT descriptor")`;
  * a size mismatch is `ZipException("invalid entry size (expected 19 but got 480 bytes)")`;
  * a CRC mismatch is
    `ZipException("invalid entry CRC (expected 0xdeadbeef but got 0xbb6f97f6)")` — the JDK prints
    the values with `Long.toHexString`, so there are no leading zeros;
  * a damaged deflate stream is reported with zlib's own message
    (`invalid code lengths set`), which is why the port reproduces zlib's tree-validity checks
    (over-subscribed / incomplete code-length sets) instead of only the RFC grammar.
* **Content rendering**: strict UTF-8 text, or `hex:<lowercase hex>` when the bytes are not valid
  UTF-8 or carry an ISO control character (the same rule the C++ side applies).

## Boundaries

* The port decodes the archive into memory per entry; the JDK streams it. The entry *contents* are
  what the importers consume, and the byte-level results are identical.
* ZIP64 archives (64-bit sizes, >4 GiB entries) are out of scope: the reference driver writes
  32-bit archives only, and the JDK's `ZipInputStream` reports those through a different path.
* The deflate decoder is a from-scratch RFC 1951 implementation (core has no third-party
  dependency). It reproduces the JDK's results on the whole corpus, including a corrupted stream
  where the JDK's zlib still produces output.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/ and fixtures/
```
