# duckduckgo golden stand

Parity pin for the native port of the DuckDuckGo Lite scraper (`DuckDuckGoScraper.kt`). Every
scenario under `scenarios/` runs through the **real Kotlin parsing pieces extracted verbatim
from the app sources**, and the canonical trace is frozen under `golden/`; the C++ port
(`agora::core::duckduckgo`, test `core/tests/duckduckgo_parity_test.cpp`) must reproduce every
trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 29 frozen edge cases across two modes |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `DuckDuckGoTraceDump.kt` | The stand driver: `search()` transcription over the real pieces |
| ``agora_extract duckduckgo`` | Verbatim extraction of the parsing pieces of the scraper |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## How the stand works

`DuckDuckGoScraper.kt` wires OkHttp through `HttpClient` and cannot compile standalone, so the
stand pins it in two halves:

* ``agora_extract duckduckgo`` copies the parsing pieces **verbatim** into the generated
  `build/ScraperHelpers.kt`: `WebResult`, the nine parsing/pagination regexes, the loop
  constants, `parseResults`, `extractUrl` and `stripHtml`. The only transformation is lifting
  class members to file level (member indentation removed) and widening `private` to
  `internal`; bodies and regex literals are untouched.
* The driver transcribes `search()` line for line over those real pieces — the page loop,
  the captcha/vqd/offset handling, the dedup counting and the partial-result fallbacks. The
  fetch itself (query/offset/vqd form values, OkHttp, HTTP statuses) is the transport
  boundary: scenario pages replace `fetchPage`, and a scenario list that runs out behaves
  like the fetch returning a null body (`Empty response body from DuckDuckGo.`).

## Scenario format (`scenarios/*.json`)

```jsonc
{ "name": "...", "mode": "parse", "html": "<a class=\"result-link\" …", "maxResults": 5 }
{ "name": "...", "mode": "search", "maxResults": 5,
  "pages": [ { "html": "…" }, { "error": "DuckDuckGo returned HTTP 503." } ] }
```

`mode: parse` records `parseResults` output only (one `result|` line per result, none when
filtered out). `mode: search` records the loop outcome: the surviving results plus the final
`response|success` or `response|error|<CAPTCHA|NETWORK_ERROR|NO_RESULTS>|<message>` line.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
mode|<parse|search>
result|<esc title>|<esc url>|<esc snippet>
response|success
response|error|<type>|<message>
```

Escaping is the shared `\\` `\p` `\s` `\n` `\r` `\t` set.

## Scope and boundaries

* Transport is out of scope: the fetchPage form encoding, OkHttp, HTTP statuses and network
  errors (scenario pages stand in for them). The query/offset/vqd form values travel to the
  `PageSource` seam and are not traced.
* Pinned Java regex semantics (all replicated in C++):
  * `find`/`findAll` are non-overlapping left-to-right scans with retries one character past
    a failed start;
  * `[^'"]` stops a capture at the FIRST quote of either kind; `[\s\S]` spans newlines;
    `.` does not (the `Unfortunately.*bots` captcha pattern is single-line);
  * `TRAILING` case rules do not apply here — all scraper patterns are case-sensitive;
  * `stripHtml` replaces the seven entities **in order**, so `&amp;lt;` double-unescapes to
    `<` (pinned);
  * `URLDecoder` maps `+` to space and `%XX` to raw bytes; invalid escapes throw on both
    sides and are outside the pinned corpus (documented, not exercised);
  * `toIntOrNull` overflow on an offset form stops the scan (the first pattern match decides).

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly
if the parsing pieces move or change shape.
