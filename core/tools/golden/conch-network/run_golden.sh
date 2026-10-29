#!/usr/bin/env bash
# Generates or verifies the golden traces for ConchNetwork.kt and BoundedWireRead.kt.
#
# Both sources compile VERBATIM from the app tree. The only substitutes are the platform halves:
#  1. okhttp3 — a stand-local subset that keeps the semantics ConchNetwork depends on
#     (case-insensitive header replacement/lookup, last-value wins, isSuccessful = 200..299,
#     per-call timeout) and records the request instead of opening a socket;
#  2. okio — a deterministic BufferedSource over scripted bytes, including `request(n)` and
#     `readUtf8LineStrict`, so the bounded-read decisions replay offline;
#  3. com.newoether.agora.api.HttpClient — the provider-wide client object; the stand supplies a
#     default OkHttpClient as the base for ConchNetwork's builder.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx jars)" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Generating scenarios..."
"${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" conch-network "$here"

sources=(
    "$app/util/ConchNetwork.kt"
    "$app/api/BoundedWireRead.kt"
    "$here/platform/HttpClient.kt"
    "$here/platform/okhttp3/OkHttp.kt"
    "$here/platform/okio/Okio.kt"
    "$here/ConchNetworkTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classpath="$jars"
echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$classpath" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/conch/network/golden/ConchNetworkTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     conch.network.golden.ConchNetworkTraceDumpKt \
     --batch "$fresh" "$here"/scenarios/*.json

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
