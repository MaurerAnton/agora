# Tool Presentation Contract

This contract defines the user-visible title and summary semantics for every built-in, MCP, and unknown tool. `ToolPresentationResolver` is the canonical lifecycle resolver and `MessageItemToolLabels` is the canonical summary renderer.

`ToolDetailPresentation` owns resource-backed detail content selection and result fields shared by
Compose and the WebUI. It consumes `ToolPresentationResolver` rather than resolving another lifecycle.
Compose retains layout, selection, prefix-aware JSON rendering, and local image loading; browser
projection stays demand-driven within watched message payloads. Failed/stopped content does not
replay completed MCP/search results, and shell exit codes remain command results rather than failures.

## Lifecycle

The visible lifecycle is deliberately small. `CALLING` and `RUNNING` share one active presentation. They must not create separate user-visible states or wording systems. Terminal presentations are completed, empty, failed, stopped, or running in background.

An exit code is a command result, not a tool failure. A completed shell call with any exit code uses `Command returned <code>`. Only transport, protocol, server, rejection, or other failures that prevent a usable command result use the failed presentation.

`wait_for_job` describes its own action after completion. Its card summary is `Waited for shell job <id>` when the ID is available. The exit code remains a compact detail status. Background summaries do not expose job IDs.

Shell job `stopping` and `settling` are nonterminal background states. `stopped` and `interrupted` are terminal stopped presentations with distinct `Stopped` and `Interrupted` summaries. These server states use the existing terminal model; nonzero command exit codes are still usable results, not transport failures.

## Wording

Display names use title case and contain no lifecycle state. Active summaries use sentence case, present-progressive wording, and a Unicode ellipsis. Completed summaries use sentence case and past tense with no terminal period. Empty summaries explicitly state that no result exists and never masquerade as ordinary completion.

Failed summaries show a concrete error reason directly whenever one is available. Remove generic `Error:` and tool-execution wrapper prefixes from the summary, capitalize the initial natural-language word, and preserve paths, identifiers and the original result/detail text. For example, `Error: command timeout` becomes `Command timeout`. Only failures without a concrete reason use a localized attempted-action fallback such as `Failed to read <path>`.

Execution failure is declared by provider error metadata or a structured protocol error, never by the spelling of successful text. Active memory, saved memory files, and skill file bodies are arbitrary content, including bodies beginning with `Error` or containing JSON error fields. Stopped summaries use past tense. Background summaries state only that the job is running in the background and do not include its ID.

Reliable subjects and counts are shown. An unavailable count is not zero and must use a count-free default. Paths, commands, file names, IDs explicitly required by an action, and user input preserve their original case. Summary text describes lifecycle only. Result content and compact detail status must not replace it.

Known counts use locale-aware Android quantity resources, including singular results. A known web result count remains visible without a query subject. Summary subjects flatten whitespace and use a maximum of 120 characters including a final Unicode ellipsis only when truncated. Full arguments and result text remain unchanged.

For memory and skill reads, effective `names` takes precedence over `name` exactly as in the provider. A single target shows its name; a batch shows `Reading/Read N files`. A streaming count is reliable only once its names array is complete. Independent `file_read` calls retain separate summaries. Empty file content and an empty conversation page explicitly say that no content/messages were returned, not ordinary completion.

The active-memory reader is a distinct display target, not an ordinary saved memory file. Its title is `Read Active Memory`, its active summary is `Reading active memory…`, and its completed summary is `Read active memory`. Empty content and failures without a concrete reason also name active memory. Concrete failures still show the reason directly. This display distinction does not change the registered `read_active_memory` API, read target, execution lifecycle, or ordinary file-read summaries.

Memory/skill rename summaries show old and new names when known; description edits name the description action. `ask_user` distinguishes waiting for answers, queued questions, answered questions and skipped questions using actual returned facts. A mixed set shows answered count against returned question count without claiming every question was answered. Question outcomes are natural sentences with quantity grammar (`Answered 1 question`, `Answered 3 questions`, `Answered 2 of 3 questions`, `Skipped 2 questions`, `Queued 2 questions`), never label-colon-count text. `stop_loop` distinguishes a new stop from an already stopped Loop.

## Localization

Every locale contains the same summary keys and placeholder types. Languages may reorder indexed placeholders. Running text uses `…`. Terminal text has no final period. A change to lifecycle semantics must update every locale and the presentation contract tests in the same change.
