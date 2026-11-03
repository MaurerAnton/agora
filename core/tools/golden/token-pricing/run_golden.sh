#!/usr/bin/env bash
# Generates or verifies the golden traces for the token-pricing cluster.
#
# Ten files compile VERBATIM from the app tree; three of them import kotlin.math only, i.e. stdlib.
# The C++ core must reproduce the traces byte for byte
# (core/tests/token_pricing_parity_test.cpp).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
tokens="$root/app/src/main/java/com/newoether/agora/api/util/tokens"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi

# Scratch lives under build/ so the whole stand's working state is git-ignored as a unit.
classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars" -d "$classes" \
    "$tokens/ImageTokenCost.kt" \
    "$tokens/PixelImageTokenCost.kt" \
    "$tokens/AnthropicModelId.kt" \
    "$tokens/AnthropicImageCost.kt" \
    "$tokens/GeminiImageCost.kt" \
    "$tokens/OpenAiImageCost.kt" \
    "$tokens/FamilyEnvelopeCosts.kt" \
    "$tokens/TextTokenCounter.kt" \
    "$tokens/LocalModelTextCounter.kt" \
    "$tokens/ContextCostModel.kt" \
    "$here/TokenPricingTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/tokens/pricing/golden/TokenPricingTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         tokens.pricing.golden.TokenPricingTraceDumpKt "$scenario" > "$fresh/$stem.trace"
done

count="$(ls "$fresh"/*.trace | wc -l)"

if [[ "${1:-}" == "--freeze" ]]; then
    rm -f "$here"/golden/*.trace
    mkdir -p "$here/golden"
    cp "$fresh"/*.trace "$here/golden/"
    echo "Frozen $count traces into $here/golden"
    exit 0
fi

if diff -r "$here/golden" "$fresh" > "$here/build/diff.txt"; then
    echo "PARITY OK: $count traces identical to the frozen golden"
else
    echo "GOLDEN DRIFT:" >&2
    head -80 "$here/build/diff.txt" >&2
    exit 1
fi
