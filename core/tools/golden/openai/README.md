# OpenAI chat stream golden stand

Parity pin for the native port of `OpenAiChatStreamConsumer`. Every transcript under
`transcripts/` is replayed through the **real Kotlin consumer** (compiled straight from the app
sources) and the canonical trace is frozen under `golden/`. The C++ port
(`agora::core::openai::ChatStreamConsumer`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `transcripts/*.txt` | One scenario per file, 91 frozen edge cases |
| `golden/*.trace` | Frozen canonical output of the Kotlin reference, one per transcript |
| `OpenAiStreamTraceDump.kt` | The reference driver: wires the Kotlin consumer to the trace format |
| `run_golden.sh` | Compiles the reference and regenerates or verifies the traces |
| `stubs/` | Leaf stubs for Android-only types the reference sources touch |
| `pow10/` | `Math.pow(10, e)` bit table and its generator (see below) |

## Transcript format

One op per line; blank lines and `#` comments are ignored.

| Op | Meaning |
| --- | --- |
| `L <line...>` | Feed one raw SSE line (the payload after `L `, verbatim) |
| `T` | A read timed out |
| `R <message...>` | A response-body read failure with this message, then stop |
| `E` | End of stream (EOF) |

## Trace format

One line per emitted event — `text|`, `thought|`, `toolCallUpdate|`, `toolCall|`, `toolCalls|`,
`usage|`, `error|` — followed by `term|`, `streamError|`, `toError|` and `describe|`. A malformed
payload also records the consumer's debug line as `log|<message>`, which carries the exception
class name the reference logs (`JsonDecodingException` / `MissingFieldException`). Text is escaped
as `\\`, `\p` (`|`), `\s` (`;`), `\n`, `\r`, `\t`; random UUIDs are normalized to `<uuid>` (dashed
form first, then 32 hex chars).

## Regenerate or verify

```sh
export KOTLINC=/path/to/kotlinc
export KOTLINX_JARS=/path/to/kotlinx-serialization-json-jvm.jar:/path/to/kotlinx-serialization-core-jvm.jar
./run_golden.sh            # verify the frozen traces still match the Kotlin reference
./run_golden.sh --freeze   # regenerate golden/*.trace
```

The C++ side consumes the same directory: `core/tests/openai_stream_parity_test.cpp` replays every
transcript through the port and compares against `golden/`, with the path injected via
`AGORA_FIXTURE_OPENAI_GOLDEN_DIR` in `core/tests/CMakeLists.txt`.

## pow10

`consumeNumericLiteral` applies the exponent as `acc * 10.0.pow(e)`. Java's `Math.pow` and the
host `std::pow` disagree by one ulp on some integer exponents, so the C++ port cannot call
`std::pow` if the JVM-frozen traces are to stay reproducible on every libm.

`pow10/ProbePow10.kt` dumps `Math.pow(10.0, e)` bit patterns for `e` in `[-400, 400]` on the
desktop JDK; the dump is frozen as `pow10/pow10_jvm.txt` and turned into
`core/src/jvm_pow10_table.generated.hpp` by `pow10/generate_table.py`. The port uses the table for
integral exponents in range and falls back to `std::pow` outside it (where every implementation
saturates to `0` or `Inf`).

Regenerate the table:

```sh
cd pow10
kotlinc ProbePow10.kt -include-runtime -d probe.jar && java -jar probe.jar > pow10_jvm.txt
python3 generate_table.py
```
