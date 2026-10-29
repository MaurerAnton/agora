# generation-policies golden stand

Parity pin for `viewmodel/GenerationPolicies.kt` and `viewmodel/AutomaticMessageApiProjection.kt` —
the rules that decide how a provider generation ends, how the UI is throttled and which history
the provider actually receives.

The two policy files are lifted **verbatim** by `agora_extract widen`, which only widens `internal`
to the default visibility so the driver can call them; no logic is changed. `ChatMessage.kt`,
`MessageSource.kt`, `PromptTemplateItem.kt` (PredefinedVariables), `RuntimeIdentity.kt`,
`api/util/ToolMessages.kt` and `Constants.kt` compile verbatim from the app tree.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 11 cases: the policy matrix, the templates, the automatic sources, the compactor, fuzz |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `GenerationPoliciesTraceDump.kt` | Driver: runs every policy of the two files |
| `agora_extract widen` | Lifts the two policy files with a widened visibility |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Extracts, compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **The terminal-disposition matrix**: every message status × pending-guidance ×
  context-compact × foreground × conversation-visible combination, for both the notification rule
  and the durable Run disposition (run status, end reason and the unread marking).
* **The two throttle gates**: the checkpoint gate (1 s, a forced boundary always writes) and the
  UI gate (50 ms, a clock rollback counts as immediately due).
* **The thought-segment tools**: `toolRoundThoughtSegments` (the clamped start index) and the
  `ToolRoundHistoryCompactor` (only a *strict* cumulative prefix is stripped; equal or unrelated
  content is retained).
* **The message templates**: the `{sent_time}` (`HH:mm:ss`), `{sent_date}` (`yyyy-MM-dd EEE`) and
  `{message_model_id}` (the model half of `Provider:model`) placeholders, the per-role prepend and
  postpend, and the rows that never get a template (protocol rows, context-compact rows, the
  automatic-input rows and empty texts).
* **The automatic-source projection**: the XML envelope the model receives instead of the stored
  text (`task_prompt`, `loop_prompt`, `ask_user_answers` with an `<unanswered/>` marker) and the
  XML escaping of the user's text.
* **The API-only history projection**: the image projections, the templates, the image stripping
  and the synthetic `api_initial_user_<parent>` row (with its `+1 ms` timestamp and the
  `Long.MAX_VALUE` carry-over).

## Boundaries

* The JVM runs with a fixed time zone (UTC) and the port formats the two date patterns in UTC, so
  the templated timestamps are comparable on both sides. The Kotlin patterns are locale-sensitive
  in principle; the corpus uses the US locale.
* The chat model is the already-ported `message_preparation::ChatMessage`; the port adds the
  structured `source` field and the segment `duration_ms` the policies read.
* The message comparison in the compactor is the Kotlin data-class equality of the fields the
  policy can observe.
