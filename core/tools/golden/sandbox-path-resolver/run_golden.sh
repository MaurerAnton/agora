#!/usr/bin/env bash
# Regenerates the golden traces for sandbox-path-resolver.
# Usage: ./run_golden.sh
# Requires: kotlinc on PATH, KOTLINX_JARS pointing to kotlinx-serialization jars.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCENARIOS_DIR="$HERE/scenarios"
GOLDEN_DIR="$HERE/golden"
BUILD_DIR="$HERE/build"
OUTPUT_DIR="$BUILD_DIR/golden"

mkdir -p "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR"

# Compile the trace dump
KOTLINC="${KOTLINC:-kotlinc}"
KOTLINX_JARS="${KOTLINX_JARS:-}"
if [[ -z "$KOTLINX_JARS" ]]; then
    echo "ERROR: KOTLINX_JARS must be set to kotlinx-serialization-core-jvm.jar:kotlinx-serialization-json-jvm.jar"
    exit 1
fi

APP_SRC="/home/user/agora-export-snapshot/app/src/fdroid/java"
UTIL_SRC="/home/user/agora-export-snapshot/app/src/main/java/com/newoether/agora/util"

echo "Compiling trace dump..."
"$KOTLINC" \
    -cp "$KOTLINX_JARS" \
    -d "$BUILD_DIR/classes" \
    "$HERE/SandboxPathResolverTraceDump.kt" \
    "$APP_SRC/com/newoether/agora/sandbox/SandboxPathResolver.kt" \
    "$APP_SRC/com/newoether/agora/sandbox/SandboxFileOperations.kt" \
    "$UTIL_SRC/ShellClient.kt" \
    "$UTIL_SRC/Constants.kt" 2>&1 | head -50

# Collect scenario files
SCENARIOS=("$SCENARIOS_DIR"/*.json)
if [[ ! -e "${SCENARIOS[0]}" ]]; then
    echo "No scenario files found in $SCENARIOS_DIR"
    exit 1
fi

echo "Running ${#SCENARIOS[@]} scenarios..."
# Run in batch mode
"$KOTLINC" -cp "$KOTLINX_JARS:$BUILD_DIR/classes" \
    -e "sandbox.path.resolver.golden.main(arrayOf(\"--batch\", \"$OUTPUT_DIR\", \"$SCENARIOS_DIR\", \"${SCENARIOS[@]}\"))"

# Move traces to golden dir
for trace in "$OUTPUT_DIR"/*.trace; do
    [[ -e "$trace" ]] || continue
    cp "$trace" "$GOLDEN_DIR/"
done

echo "Golden traces written to $GOLDEN_DIR"