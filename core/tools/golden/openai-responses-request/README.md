# openai-responses-request golden stand

Parity pin for `api/openai/OpenAiRequestValidator.kt` — the OpenAI Responses request builder and
the local wire-format gate — together with `api/util/SafeWireNames.kt` and
`api/util/RequestFormatValidation.kt`.

The validator, the wire-name grammars and the request-format gate compile **verbatim from the app
tree**. The four Responses request models (`OpenAiResponsesRequest`, `OpenAiResponseInputItem`,
`OpenAiResponseInputContent`, `OpenAiResponseTool`) are lifted byte-for-byte out of
`api/LlmProvider.kt` by `agora_extract responses-models`: that file is the provider abstraction and
pulls in the message-preparation and cost-model clusters, none of which these models need.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 54 cases: the input builder, the tool conversion and the wire-format gate |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `OpenAiResponsesRequestTraceDump.kt` | Driver: builds the chat messages / tools / request and renders the result |
| `agora_extract responses-models` | Lifts the four request models out of `api/LlmProvider.kt` |
| `agora_scenarios openai-responses-request <stand>` | Deterministic scenario generator |
| `run_golden.sh` | Extracts, compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **The input builder** (`toResponsesInput`): a tool message becomes a `function_call_output`, a
  chat message becomes a `message` item with `input_text`/`output_text` per role, an image part
  becomes an `input_image` with `detail = "auto"`, and a message without content produces no item.
* **The reasoning replay rule**: the stored Responses output items are replayed **only** when the
  provider matches *and* the stored model matches the target model (`models/` prefix and case
  insensitive); otherwise the plain `function_call` items are rebuilt from the stored tool calls.
  The model comparison uses `ModelId.parse(...).modelName`, so a stored `Provider:model` id is
  compared by its model half.
* **The tool conversion** (`toResponsesTools`): the chat tool definitions become Responses tools
  with the function's name, description and parameters.
* **The wire-format gate** (`requireValidWireFormat`): every violation is collected and reported
  together — a blank model, an empty input, a non-positive `max_output_tokens`, a `top_p` outside
  0..1, an unsafe or unsupported tool, an invalid message role, a message that interrupts a pending
  tool result, empty content, the input/output text types that do not match the role, an
  `input_image` without a detail, a non-wire-safe `call_id`/`name`, a reused `call_id`, arguments
  that are not a JSON object, a function output without a pending call, a missing output, an item
  carrying unrelated fields, and the request that does not end in a user message or a function
  output. The exception message is the provider plus the distinct violations joined with `"; "`.
* **The two wire-name grammars**: `[A-Za-z0-9_-]{1,64}` for a tool name and
  `[A-Za-z0-9_-]{1,128}` for a call id.

## Boundaries

* A malformed input item is reported with the same violation prefix
  (`input[N] is not a supported Responses item`); the kotlinx diagnostic that follows the prefix is
  not reproduced (the corpus keeps the items well-typed).
* The blank checks follow Kotlin's `isNullOrBlank` with ASCII whitespace; the corpus uses ASCII
  whitespace only.
* The tool JSON in the trace is rendered with the provider's request encoder
  (`encodeDefaults = true`, `explicitNulls = false`), so the trace shows the wire form the app
  sends.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
