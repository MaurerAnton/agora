# shell-file-edit golden stand

Parity pin for the pure file helpers at the top of `util/ShellClient.kt`: the file-edit matcher
and replacer, line-ending preservation, line counting, SHA-256 and the bounded shell-output
reader. The Conch/SSH transport, file IO and the process stream stay on the platform side.

`ShellClient.kt` is the Conch shell transport object. `extract_shell_file_edit.py` copies the
declarations out of it verbatim; only the three private helpers are widened to `internal` so the
golden driver can call them. `ShellFileReadResult` and `shellFileLineCount` are also extracted
for the cross-check, but their C++ port already lives in `shell_tools.hpp` and is pinned by the
separate `shell-tools` stand, so this stand does not duplicate the C++ model or line counter.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 171 frozen scenarios |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `ShellFileEditTraceDump.kt` | The stand driver |
| `extract_shell_file_edit.py` | Lifts the declarations out of `ShellClient.kt` |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |

## What is pinned

- **Line count** (`shellFileLineCount`): 0 for empty content, otherwise the `'\n'` count plus one;
  CRLF, lone CR, trailing newline and Unicode content. The C++ call goes through
  `shell_tools::shell_file_line_count`, the port already pinned by `shell-tools`.
- **SHA-256** (`shellFileSha256`): `MessageDigest("SHA-256")` over the exact bytes, lowercase
  two-digit hex per unsigned byte (`"%02x".format(Byte)`); empty, known vectors, NUL bytes, all
  256 byte values and a 1000-byte payload.
- **UTF-8 prefix completion** (`completeUtf8PrefixLength`): complete 1/2/3/4-byte sequences,
  truncated sequences of each length, continuation-only buffers, invalid leads (`FF`, `C0`),
  overlong `C0 80`, stray continuations and a complete sequence followed by stray bytes.
- **Bounded shell output** (`readBoundedShellOutput`): at/under/over the limit, zero limit, the
  default 1 MiB limit, a cut in the middle of each UTF-8 sequence length, the incomplete trailing
  sequence dropped only when truncated, and the JDK REPLACE-mode decoder for malformed input
  (one `U+FFFD` per malformed maximal subpart, re-examining the remaining octets). The scripted
  `InputStream` pins multi-chunk reads; the C++ policy takes the already-collected bytes because
  chunking cannot change the result.
- **Match ranges** (`fileEditMatchRanges`): CRLF in content and needle normalized to LF before
  searching, ranges returned in the original UTF-16 units, a matched CRLF pair included as two
  units, non-overlapping matches (`aaaa`/`aa`, `abababa`/`aba`), lone CR, Unicode/emoji indices,
  needle longer than content, empty content and the exact `require` failure for an empty needle.
- **Replacement** (`replaceFileEditMatches`): line ending chosen from the matched text, then the
  file, then the replacement text, then `"\n"`; CRLF normalization of the replacement; lone CR
  preserved; ties resolved by the first line ending; empty ranges; and the exact `require`
  failure for overlapping/out-of-order or out-of-bounds ranges.
- **Edit decision** (`editShellFileContent`): not-found and multiple-match errors, `replaceAll`
  selection, replacement counts, the default empty `sha256` and null error, CRLF/LF preservation
  and empty old-string rejection.
- **Line-ending preference** (`preferredFileEditLineEnding`): CRLF/LF majority, ties use the
  first ending, lone CR ignored, no ending -> null.
- **Line-ending application** (`withFileEditLineEnding`): no LF -> unchanged; otherwise CRLF
  normalize and apply the requested ending, including empty and lone-CR endings.

## Scenario format

```json
{ "op": "edit", "content": "a\r\nb", "oldString": "b", "newString": "x\ny",
  "replaceAll": false }
```

Byte inputs use hex so invalid UTF-8 is expressible:

```json
{ "op": "readBoundedShellOutput", "bytesHex": "e282", "maxBytes": 2, "chunks": [1, 1] }
```

`chunks` is always echoed; an empty list means `ByteArrayInputStream` (reads are capped at
`DEFAULT_BUFFER_SIZE`, 8192). Other ops are `lineCount`, `sha256`, `completeUtf8Prefix`,
`matchRanges`, `replaceMatches`, `preferredLineEnding` and `withLineEnding`.

## Trace format

One compact `JsonObject.toString()` line per scenario, e.g.

```json
{"op":"edit","content":"a\r\nb\r\nc","oldString":"b","newString":"x\ny","replaceAll":false,"outcome":{"content":"a\r\nx\r\ny\r\nc","result":{"replacements":1,"sha256":"","error":null}}}
```

The C++ side builds the same object with `json::to_kotlin_string` (insertion order preserved) and
the test compares the bytes, including the trailing newline. Kotlin `IntRange` is echoed as
`[first,last]`, both ends inclusive.

## Scope and boundaries

- The extracted Kotlin is `SHELL_COMMAND_OUTPUT_MAX_BYTES`, `ShellFileReadResult`,
  `ShellFileEditResult`, `shellFileLineCount`, `readBoundedShellOutput`,
  `completeUtf8PrefixLength`, `fileEditMatchRanges`, `replaceFileEditMatches`,
  `editShellFileContent`, `shellFileSha256`, `preferredFileEditLineEnding` and
  `String.withFileEditLineEnding`. `String.shellUtf8Prefix` is not re-extracted; the `conch-sse`
  stand already pins it.
- `readBoundedShellOutput` reads an `InputStream` in Kotlin. The core port takes the collected
  byte string; the platform adapter performs the chunked read. The traces pin that the result is
  independent of chunk boundaries, including a stream that returns one byte at a time.
- `new String(bytes, UTF_8)` uses the JDK's REPLACE-mode decoder. The shared
  `core/src/java_utf8.hpp` decoder is the same code that `java.net.URI`'s escaped-component port
  uses; malformed-input traces pin one `U+FFFD` per malformed maximal subpart.
- `ShellFileReadResult` and `shellFileLineCount` are already ported in `shell_tools.hpp`; the
  line-count traces here are a cross-check, not a second implementation. The C++ file-edit API
  uses `std::u16string` because Kotlin `String` indices and `IntRange` endpoints are UTF-16 code
  units; `utf16::from_utf8` / `utf16::to_utf8` are the test-boundary conversions.
- File IO, Conch/SSH, sockets and process execution remain platform concerns.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc \
KOTLINX_JARS=core.jar:json.jar ./run_golden.sh            # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc \
KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze   # regenerate
python3 gen_scenarios.py                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (git-ignored scratch, including the extracted
`ShellFileEdit.kt`) before each run.
