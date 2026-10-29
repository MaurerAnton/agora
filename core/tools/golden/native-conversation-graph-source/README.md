# native-conversation-graph-source

Golden stand for `data/NativeConversationGraphSource.kt` — the v5 graph spool (index-driven
split of `conv/items/*.json` into one JSON array file per graph field) and the v<5 legacy pass
through.

## What compiles verbatim

- `NativeConversationGraphSource.kt` (including the `NativeGraphEntrySource` interface it declares)
- `NativeBackupFormat.kt` — the `NativeConversationIndex` decode and entry vocabulary, with the
  same settings model cluster the `native-backup-format`/`native-backup-archive` stands use
- `InputStream.readToEnd()` — the four-line drain helper, lifted byte-for-byte from
  `NativeBackupArchive.kt` by `agora_extract read-to-end` (compiling the whole archive class would
  drag in the `Context`/`Uri`/`ParcelFileDescriptor` surface this stand does not substitute)

## Platform substitutes

1. **`android.util.JsonReader` / `JsonWriter` / `JsonToken` / `JsonScope` /
   `MalformedJsonException` + `com.android.internal.util.StringPool`** — vendored **verbatim**
   from `android.googlesource.com` (`platform/frameworks/base`, `core/java/android/util/…`,
   fetched from `refs/heads/main`) in `vendor/android-util-json/`. The single documented
   build-time edit (`prepare_vendor.py`) drops the `@android.ravenwood.annotation…` lines,
   which only exist for Android's internal test harness. `javac` compiles them into the stand
   classpath, so the streaming parse, the strict-mode acceptance rules, the `line N column M`
   error arithmetic and the `Double.toString` emission the spool re-serialisation depends on
   are Android's own code, not a reimplementation.
2. **`NativeGraphEntrySource`** — an in-memory/file-backed double: `has()`/`bytes()` serve the
   scenario entries, `stream()` hands out a `FileInputStream` over the same bytes. Scenarios may
   mark a stream `null` (`has` true, stream missing) or give it a **drain fault**: at its end it
   throws instead of reporting EOF — the behaviour a checked archive stream has at a size/CRC
   mismatch, which the source's parse-then-`readToEnd` ordering must propagate.
3. The DataStore `SettingsManager` double + `Preferences`/`SecretCrypto`/`DebugLog` stubs exist
   only so `SettingsContracts.kt` compiles, same as the archive stand.

## Trace surface

Each step reports:

- `result.content` (full open) or `result.firstByte` (partial read), plus
  `result.readAfterClose` — what reading the handed-out stream does **after** `close()`:
  the JDK contracts differ by construction (a legacy `FileInputStream` throws
  `IOException: Stream Closed`, the spool's `SequenceInputStream` reports `eof` because close
  nulls the current element), and both are pinned;
- `result.error` — type + message for every failure path: `require`/`error`/`checkNotNull`
  messages, kotlinx index decode (`MissingFieldException`, `JsonDecodingException` including the
  `JSON input:` suffix), Android's `MalformedJsonException … at line N column M`,
  `EOFException: End of input`, and a drain fault propagating after a successful parse;
- `cache` — the cache directory's remaining children after the step: **always empty**, which
  pins spool cleanup on the success, validation-failure and read-failure paths.

Scenario surface: version branch (legacy vs index), duplicate-id/entry-name validation,
missing index/item/tasks entries, the split itself (field routing, the in-item `tasks` skip,
unknown-field skip, value copying with Android's number/string re-emission), empty graphs,
trailing bytes after the value (drained, not rejected) and raw control characters inside
strings (accepted, re-escaped on write).

## Running

```sh
JAVA_HOME=<jdk17> KOTLINC=<kotlinc-with-serialization-plugin> \
KOTLINX_JARS=<kotlinx-serialization-core+json+jvm|coroutines jars> \
./run_golden.sh --freeze   # regenerate traces (first run)
./run_golden.sh            # verify against the frozen traces
```

`javac` (any modern JDK) must be on `PATH`; it compiles the vendored Android sources only.
