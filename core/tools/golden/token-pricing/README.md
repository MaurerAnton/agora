# token-pricing golden stand

Parity pin for the native port of the token-pricing cluster. Every scenario under `scenarios/`
runs through the **real Kotlin sources compiled verbatim from the app tree** and the canonical
trace is frozen under `golden/`; the C++ port (`agora::core::token_pricing`, header
`core/include/agora/core/token_pricing.hpp`, implementation `core/src/token_pricing.cpp`, test
`core/tests/token_pricing_parity_test.cpp`) must reproduce every trace byte for byte.

## What the stand compiles, and why

| File | Role |
| --- | --- |
| `api/util/tokens/ImageTokenCost.kt` | `ImageDescriptor`, `ByteProportionalImageCost` |
| `api/util/tokens/PixelImageTokenCost.kt` | pixel fallback, `blocksCovering`, `proportionalEdge` |
| `api/util/tokens/AnthropicModelId.kt` | id parsing and `versionAtLeast` |
| `api/util/tokens/AnthropicImageCost.kt` | 28x28 visual tokens with tier caps |
| `api/util/tokens/GeminiImageCost.kt` | fixed 258-token crops and the crop-unit formula |
| `api/util/tokens/OpenAiImageCost.kt` | patch/tile pricing and the published model table |
| `api/util/tokens/FamilyEnvelopeCosts.kt` | `EnvelopeCost`, `SafetyMargin`, Anthropic/OpenAI framing |
| `api/util/tokens/TextTokenCounter.kt` | `HeuristicTextTokenCounter` |
| `api/util/tokens/LocalModelTextCounter.kt` | exact-with-fallback counter |
| `api/util/tokens/ContextCostModel.kt` | `ContextCostModel.Default` wiring |

All ten files are host-compilable; the only imports are `kotlin.math` in three of them (stdlib),
so the stand needs no stubs and no source extraction. The estimator and family mapping above this
cluster (`ContextCostModels.kt`, `CostModelContextEstimator.kt`) and the BPE counters are out of
scope here.

## What is pinned

* **heuristic counter** — empty text is 0; ASCII letter/digit runs cost `ceil(run / 4)` and only
  terminate on whitespace or another code point; every other code point costs one token. The
  boundary cases are the point: an NBSP is *not* `Character.isWhitespace`, so `a<NBSP>b` is three
  tokens; emoji (supplementary plane) cost one token per code point, not per UTF-16 unit; accented
  letters flush the run and cost one each.
* **local counter** — empty text short-circuits to 0 *without consulting the exact method*; an
  exception from it falls back to the heuristic; a negative exact count is a failure report and
  falls back; zero is a valid exact count.
* **byte fallback** — no usable byte size (absent, zero, negative) costs the historical 1024;
  otherwise `bytes / 768` clamped to `[256, 1536]`, pinned at both clamps and just off them.
* **Anthropic images** — the tier comes from the id parser (`>= 4.7` is high resolution), the
  28 px patch count is capped by the tier's long edge *and* visual-token budget, and the "largest
  size that fits" is found by the same bisection the Kotlin uses. Pinned at both tier caps, at a
  downscale-by-long-edge case (5000x1000), and through the byte fallback when a dimension is
  missing or zero.
* **id parsing** — `parse` is regex-driven: a version pair (`4-5` or `4.5`) never matches inside a
  longer number (a release date is not a version), `4.123456` degrades to major-only, a trailing
  `.1` does not hide `4.5`, and a tier is decided by the `opus`/`sonnet`/`haiku` substring after
  the last path segment. `versionAtLeast` is false for an unreadable version.
* **Gemini images** — both dimensions at most 384 px is one tile; otherwise the crop unit is
  `floor(min / 1.5)` (floored to 1) divided into *both* dimensions, so the guide's own 960x540
  example is 3x2 tiles, a 1000x1 image pins the crop-unit-1 floor, and three 4:3 cases
  (400x300, 300x400, 800x600) pin the 1.5 divisor itself — any other divisor tiles them
  differently.
* **OpenAI images** — the whole published table in its dispatch order (mini before the family,
  nano before 4.1, unknown ids over-estimate with the 1.2 default), the tile rule (2048 square,
  short edge 768, 512 px tiles, per-model base), and the patch rule including the shrink
  correction path (pinned for gpt-5.4, gpt-5.5 and the unknown default, plus non-square inputs
  where whole-patch truncation actually changes the scaled dimensions — square inputs shrink to
  exact integer patches and would not catch a dropped correction).
* **envelope costs** — the shared default and OpenAI framing as `toString()`, and the Anthropic
  tool-use table by (tier, major*10+minor), including the largest-published fallback for
  unrecognised ids, unknown vendors and missing tiers.
* **safety margin** — round up at 11/10 (so small counts never lose the margin), clamp to
  `[0, Int.MAX_VALUE]` from below (negative raw counts) and above (2e9).
* **default bundle** — `ContextCostModel.Default` is exercised as a whole: heuristic text, byte
  images, default envelope, ten percent margin.

## Boundary note

`AnthropicModelId.parse` and `OpenAiImageCosts.forModelName` call `String.lowercase()`
(Unicode-aware). Like the other core ports, this implementation folds **ASCII A–Z only**; model
ids are ASCII in practice and the scenarios use ASCII letters. The heuristic counter classifies
ASCII code points exactly as `Character.isLetterOrDigit` does below 0x80 and uses the JVM
whitespace table for every code point, which the NBSP scenario pins.

## Scenario format (`scenarios/*.json`)

One operation per file; `op` selects it:

```json
{"name": "anthropic_standard_cap", "op": "anthropicImage",
 "model": "claude-opus-4-6", "image": {"byteSize": null, "pixelWidth": 1568, "pixelHeight": 1568}}
```

| `op` | Fields | Result |
| --- | --- | --- |
| `heuristic` | `text` | tokens |
| `local` | `text`, `exact` (nullable), `throws` | tokens |
| `byteImage` | `image` | tokens |
| `anthropicTier` | `model` | `STANDARD` / `HIGH_RESOLUTION` |
| `anthropicImage` | `model`, `image` | tokens |
| `geminiImage` | `image` | tokens |
| `openaiImage` | `model`, `image` | tokens |
| `anthropicModelId` | `model` | `AnthropicModelId.toString()` |
| `versionAtLeast` | `model`, `major`, `minor` | `true` / `false` |
| `envelopeDefault` | — | `EnvelopeCost.toString()` |
| `envelopeOpenAi` | — | `EnvelopeCost.toString()` |
| `envelopeAnthropic` | `model` | `EnvelopeCost.toString()` |
| `margin` | `raw` | corrected count |
| `defaultModel` | `text`, `image`, `raw` | `text\|image\|envelope\|margin` |

The `image` object is always present with all three fields; null means "unknown". Descriptors are
printed as Kotlin data classes in the `args` line, which also pins `ImageDescriptor.toString()`.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
op|<operation>
args|<operation-specific arguments joined by |>
result|<operation result>
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`. A null argument prints `-`
and an empty string is escaped as itself (so `heuristic_empty` has an empty `args|` payload).

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent. The
C++ test reads `scenarios/` and `golden/` straight from this directory through
`AGORA_FIXTURE_TOKEN_PRICING_DIR`.
