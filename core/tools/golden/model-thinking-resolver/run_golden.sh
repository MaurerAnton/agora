#!/usr/bin/env bash
# Generates or verifies the golden traces for data/ModelThinkingCapabilityResolver.kt.
#
# The resolver, ModelId, CustomProviderConfig and the whole thinking-capability cluster compile
# VERBATIM from the app tree. CustomProviderIdentity.kt cannot compile host-side (Room/model
# migration + UI imports); extract_identity_policy.py copies the stable-id slice used by
# CustomProviderConfig.providerId out of it byte for byte on every run.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against golden/
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the traces
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
python3 "$here/extract_identity_policy.py" \
    "$app/data/CustomProviderIdentity.kt" "$here/build/CustomProviderIdentityPolicy.kt"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" \
    "$app/util/Constants.kt" \
    "$app/model/ThinkingControl.kt" \
    "$app/model/ModelThinkingCapability.kt" \
    "$app/model/ModelThinkingCapabilityDefaults.kt" \
    "$app/model/ModelThinkingCapabilities.kt" \
    "$app/model/ModelId.kt" \
    "$app/data/CustomProviderConfig.kt" \
    "$here/build/CustomProviderIdentityPolicy.kt" \
    "$app/data/ModelThinkingCapabilityResolver.kt" \
    "$here/ModelThinkingResolverTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/model/thinking/resolver/golden/ModelThinkingResolverTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         model.thinking.resolver.golden.ModelThinkingResolverTraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
