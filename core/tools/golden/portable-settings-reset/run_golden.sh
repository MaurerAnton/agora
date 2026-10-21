#!/usr/bin/env bash
# Generates or verifies the golden traces for data/PortableSettingsReset.kt.
#
# PortableSettingsReset.kt and data/SettingsPreferenceSchema.kt compile VERBATIM from the app tree;
# the only platform substitute is androidx.datastore.preferences.core (the in-memory preference
# store), which is the documented leaf stub of the native-backup-secrets stand.
#
# Usage:
#   ./run_golden.sh            # verify
#   ./run_golden.sh --freeze   # regenerate traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
jars="${KOTLINX_JARS:-}"

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Regenerating the settings schema key list..."
python3 "$here/gen_schema_keys.py"

sources=(
    "$app/data/SettingsPreferenceSchema.kt"
    "$app/data/PortableSettingsReset.kt"
    "$here/platform/Preferences.kt"
    "$here/SchemaKeys.kt"
    "$here/PortableSettingsResetTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars" -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/portable/settings/reset/golden/PortableSettingsResetTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
     portable.settings.reset.golden.PortableSettingsResetTraceDumpKt \
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
