# malformed-tool-call golden stand

Parity pin for `api/util/MalformedToolCall.kt` — the stand-in for a tool call the model emitted in a
form that cannot be executed or paired.

The policy file is lifted **verbatim** by `extract_policy.py`, which only widens `internal` to the
default visibility so the driver can call it; `api/StreamEvent.kt` and its models compile verbatim
from the app tree.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 5 cases: the stand-in request and the error text the model receives |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `MalformedToolCallTraceDump.kt` | Driver: builds the stand-in and renders it |
| `extract_policy.py` | Lifts the policy file with a widened visibility |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Extracts, compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **The stand-in request**: the reserved tool name, the `call_malformed_<suffix>` id, the arguments
  object (the cause plus the original name and arguments when they are not empty, truncated to 200
  and 2000 characters), the preserved provider signature and the stream key (the caller's key when
  it is not blank, otherwise `malformed_tool_<suffix>`).
* **The error text**: the generic explanation plus the cause, the original name and the original
  arguments when the arguments document carries them. A malformed document (not JSON, not an
  object) yields the generic text only, and a field that is not a primitive is ignored — a number
  is a primitive and contributes its raw text.
* The random UUID suffix is **entropy**: the Kotlin source calls `UUID.randomUUID()`. The port takes
  the suffix as a parameter (the caller owns the generator) and the stand masks it, so the two
  sides stay comparable without hiding the rest of the contract.

## Boundaries

* The stand-in request mirrors `api/StreamEvent.kt::ToolCallRequest`; the port returns the same
  fields without the sealed-class hierarchy.
* The JSON encoding of the arguments uses the already-pinned kotlinx-compatible writer.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
