# conversation-runtime golden stand

Parity pin for the native port of the conversation runtime reducer. Every scenario under
`scenarios/` runs through the **real Kotlin cluster compiled verbatim from the app tree** and the
canonical trace is frozen under `golden/`; the C++ port
(`agora::core::conversation_runtime`, header `core/include/agora/core/conversation_runtime.hpp`,
implementation `core/src/conversation_runtime.cpp`, test
`core/tests/conversation_runtime_parity_test.cpp`) must reproduce every trace byte for byte.

## What the stand compiles, and why

`model/ConversationRuntimeReducer.kt` is not standalone; it is the top of a small type cluster:

| File | Role |
| --- | --- |
| `model/RuntimeIdentity.kt` | `RunStatus`, `RunEndReason`, `RuntimeRunIdentity`, `RunEffectIdentity`, `RunRecoverySnapshot` |
| `model/RunEffect.kt` | `SlotReleaseReason` and the `RunEffect` catalogue |
| `model/RuntimeTransition.kt` | `Transition`, `CommandRejection`, `RunProviderPhase`, `RunToolPhase` |
| `model/ConversationCommand.kt` | the command catalogue |
| `model/RunState.kt` | the state catalogue and its `require` invariants |
| `model/ConversationRuntimeReducer.kt` | the reducer itself |

Every one of these files has **no imports at all** — no Android, no coroutines, no Room, no
kotlinx — so the whole cluster compiles as written. This stand needs no stubs and no source
extraction; the driver (`ConversationRuntimeTraceDump.kt`) is the only additional source.

## What is pinned

The traces pin the reducer's decisions *and the order in which it makes them*:

* **conversation routing first** — a command whose conversation id differs from the state's is
  `STALE_IDENTITY` before its kind is inspected;
* **recovery** — `Recover` is accepted only from `Idle`; the planned effect identity is
  `recover-<runId>-<pass>` with `ownerToken = Long.MAX_VALUE`; a repeated snapshot is
  `DUPLICATE_RESULT`, a different one `STALE_IDENTITY`; success returns to `Idle`, failure is
  reported exactly once;
* **slot acquisition** — `AcquireSlot` is accepted only from `Idle` and emits `SlotActivated`;
* **Send placement and guidance** — from `Idle` a Send enters `Preparing` (owner identity with
  `runId = null`); from an unbound `Active` it is deferred (`AwaitRunRelease`); a direct Send is
  rejected as busy (`RejectSendBusy`) whenever the slot is held; pending guidance from `Idle`
  emits `DrainGuidanceFirst` unless the Send is direct;
* **input acceptance** — `InputPersisted` binds the durable Run (from `Preparing`, or after a Stop
  through `bindDurableRunAfterStop`, which only proceeds on a settled, effect-free `Stopping`);
  `InputPersistenceFailed` reports once; `SendLaunchAbandoned` releases the slot with
  `SEND_LAUNCH_ABANDONED`;
* **Run binding** — the same owner may bind a Run once, a second Run while one is bound is
  `STALE_IDENTITY`, a repeated identity is `DUPLICATE_RESULT`, and a bound Run only advances by
  exactly one pass;
* **provider passes** — one pass at a time, accepted only with no open tool phase and no settled
  coroutine; the same identity again is `DUPLICATE_RESULT`, a different one `STALE_IDENTITY`;
  completion emits `ProviderPassAccepted` and closes the phase;
* **tool gate** — the batch effect id is derived as `tool-batch-<request id>` and the commit as
  `tool-round-<batch effect id>`; the completion carries the derived id back (the raw request id
  is stale); a failed commit is reported once and keeps the phase open; success closes it and
  emits `ContinueProviderPass`;
* **natural finalization** — two barriers (`FinalizeRun` persistence and coroutine), releasing the
  slot with `NORMAL_FINALIZATION_SETTLED` only when both have settled, in either arrival order;
  a reported persistence failure re-arms only through a Stop retry, never through another success
  (the `RunState.Finalizing` invariant forbids that combination, so no scenario can construct it);
* **Stop** — always cancels the pass (`CancelProviderPass`) and adds `FinalizeStop` when
  persistence is required; an already-settled coroutine without persistence releases immediately
  with `EMPTY_STOP`; a Stop that raced the input bind re-arms the persistence barrier through
  `FinalizeStop(stop-<ownerToken>)`; retrying after a reported persistence failure clears the flag
  and re-emits `FinalizeStop`; both barriers release with `STOP_BARRIERS_SETTLED`;
* **identity algebra** — effect identities derive from run identities field by field; `runId` is
  `null` only between the slot claim and the input/bind step, and the traces distinguish `null`
  from an empty string;
* **every rejection the reducer can produce** — `ILLEGAL_STATE`, `STALE_IDENTITY`,
  `DUPLICATE_RESULT` — with the state that produced it.

## Derived effect ids

Four ids are computed by the reducer rather than carried in a command:

| Effect | Id |
| --- | --- |
| recovery plan | `recover-<runId>-<pass>` |
| tool batch | `tool-batch-<request effectId>` |
| tool round commit | `tool-round-<batch effectId>` |
| Stop re-armed by a bind-after-stop | `stop-<ownerToken>` |

## Scenario format (`scenarios/*.json`)

```json
{"name": "stop_bound_persistence_then_coroutine", "conversationId": "c", "commands": [
  {"kind": "AcquireSlot", "identity": {"conversationId": "c", "ownerToken": 1, "pass": 0}},
  {"kind": "StopRequested", "identity": {"conversationId": "c", "ownerToken": 1},
   "coroutineAlreadySettled": false, "requiresPersistence": true, "effectId": "stop"}]}
```

Every scenario starts from `RunState.Idle(conversationId)` and replays `commands` in order. The
identity defaults mirror the driver: effect identities default to `ownerToken = 1`, `pass = 0`,
`effectId = "e"` (and `runId = "run"` where the command reads a run), runtime identities default to
`ownerToken = 1`, `pass = 0` and carry **no** default `runId` — an absent `runId` is `null`.
Booleans default to `false`; `StopRequested.effectId` is nullable.

A command that violates a Kotlin `require` in the command or snapshot type aborts the driver, so the
frozen set is also a proof that the public command surface accepts every sequence it contains.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
initial|Idle(conversationId=c)
cmd|<i>|<command>
state|<i>|<state after the command>
effects|<i>|[<emitted effects>]
reject|<i>|- | ILLEGAL_STATE | STALE_IDENTITY | DUPLICATE_RESULT
```

`<command>`, `<state>` and `<emitted effects>` are Kotlin data-class `toString()` output, printed
deliberately: every declared field, in declaration order, including nested identities and phase
objects, is pinned by the trace. The C++ `describe(...)` overloads reproduce that format exactly, so
a port that drops, renames or reorders a field fails instead of silently drifting. The driver also
prints the command it built, which pins the C++ scenario parser against the same schema.

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
../../../build/agora_scenarios conversation-runtime .   # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent. The
C++ test reads `scenarios/` and `golden/` straight from this directory through
`AGORA_FIXTURE_CONVERSATION_RUNTIME_DIR`.
