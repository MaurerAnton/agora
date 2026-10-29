#!/usr/bin/env bash
# Generates or verifies the golden traces for the provider-protocol helpers in
# data/CustomProviderConfig.kt.
#
# CustomProviderConfig.kt and Constants.kt compile VERBATIM from the app tree. The identity slice
# that provides CustomProviderIdentityPolicy.isStableId is extracted from the real
# CustomProviderIdentity.kt by the sibling model-thinking-resolver stand's script.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh [--freeze]
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting the identity-policy stable-id slice..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" identity-policy \
    "$app/data/CustomProviderIdentity.kt" "$here/build/CustomProviderIdentityPolicy.kt"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" \
    "$app/util/Constants.kt" \
    "$app/data/CustomProviderConfig.kt" \
    "$here/build/CustomProviderIdentityPolicy.kt" \
    "$here/ProviderProtocolTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/provider/protocol/support/golden/ProviderProtocolTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         provider.protocol.support.golden.ProviderProtocolTraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
