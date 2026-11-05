#!/usr/bin/env bash
# Regenerates the golden traces for sandbox-path-resolver.
# Usage: ./run_golden.sh
# Requires: kotlinc on PATH, KOTLINX_JARS pointing to kotlinx-serialization jars.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$HERE/../../../.." && pwd)"
SCENARIOS_DIR="$HERE/scenarios"
GOLDEN_DIR="$HERE/golden"
BUILD_DIR="$HERE/build"
OUTPUT_DIR="$BUILD_DIR/golden"

mkdir -p "$BUILD_DIR" "$OUTPUT_DIR"

# Compile the trace dump
KOTLINC="${KOTLINC:-kotlinc}"
KOTLINX_JARS="${KOTLINX_JARS:-}"
if [[ -z "$KOTLINX_JARS" ]]; then
    echo "ERROR: KOTLINX_JARS must be set to kotlinx-serialization-core-jvm.jar:kotlinx-serialization-json-jvm.jar"
    exit 1
fi

APP_SRC="$root/app/src/fdroid/java"
UTIL_SRC="$root/app/src/main/java/com/newoether/agora/util"

android_classes="$BUILD_DIR/android-classes"
mkdir -p "$android_classes"

# The sandbox surface now reaches the shell client: android.util.Base64 (vendored from the
# shell-crypto stand) and the ConchChannelException family (extract profile) come in as platform
# substitutes, exactly like the conch-network stand.
javac -nowarn -d "$android_classes" "$root/core/tools/golden/shell-crypto/platform/Base64.java"
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" conch-exceptions \
    "$UTIL_SRC/ConchEncryptedHttp.kt" "$BUILD_DIR/ConchExceptions.kt"

echo "Compiling trace dump..."
"$KOTLINC" \
    -cp "$KOTLINX_JARS:$android_classes" \
    -d "$BUILD_DIR/classes" \
    "$HERE/SandboxPathResolverTraceDump.kt" \
    "$APP_SRC/com/newoether/agora/sandbox/SandboxPathResolver.kt" \
    "$APP_SRC/com/newoether/agora/sandbox/SandboxFileOperations.kt" \
    "$APP_SRC/com/newoether/agora/sandbox/SandboxVirtualPaths.kt" \
    "$UTIL_SRC/ShellClient.kt" \
    "$UTIL_SRC/ShellCrypto.kt" \
    "$UTIL_SRC/PortableGlobMatcher.kt" \
    "$UTIL_SRC/ShellFileText.kt" \
    "$UTIL_SRC/ShellFileEditSnippets.kt" \
    "$UTIL_SRC/NetworkRetryPolicy.kt" \
    "$UTIL_SRC/NetworkHttpRetry.kt" \
    "$UTIL_SRC/ConchNetwork.kt" \
    "$root/app/src/main/java/com/newoether/agora/api/BoundedWireRead.kt" \
    "$root/app/src/main/java/com/newoether/agora/viewmodel/StreamScope.kt" \
    "$root/core/tools/golden/conch-network/platform/HttpClient.kt" \
    "$root/core/tools/golden/conch-network/platform/okhttp3/OkHttp.kt" \
    "$root/core/tools/golden/conch-network/platform/okio/Okio.kt" \
    "$BUILD_DIR/ConchExceptions.kt" \
    "$UTIL_SRC/Constants.kt" \
    "$HERE/stubs/android/system/Os.kt" 2>&1 | head -50

# Collect scenario files
SCENARIOS=("$SCENARIOS_DIR"/*.json)
if [[ ! -e "${SCENARIOS[0]}" ]]; then
    echo "No scenario files found in $SCENARIOS_DIR"
    exit 1
fi

echo "Running ${#SCENARIOS[@]} scenarios..."
# Run in batch mode
"$KOTLINC" -cp "$KOTLINX_JARS:$BUILD_DIR/classes:$android_classes" \
    -e "sandbox.path.resolver.golden.main(arrayOf(\"--batch\", \"$OUTPUT_DIR\", \"$SCENARIOS_DIR\", \"${SCENARIOS[@]}\"))"

# Verify against the frozen traces; --freeze replaces them (every other stand's contract — the
# old script overwrote the golden on every run).
if [[ "${1:-}" == "--freeze" ]]; then
    mkdir -p "$GOLDEN_DIR"
    rm -f "$GOLDEN_DIR"/*.trace
    for trace in "$OUTPUT_DIR"/*.trace; do
        [[ -e "$trace" ]] || continue
        cp "$trace" "$GOLDEN_DIR/"
    done
    count=$(ls "$GOLDEN_DIR"/*.trace | wc -l)
    echo "Frozen $count traces into $GOLDEN_DIR"
    exit 0
fi

if diff -r "$GOLDEN_DIR" "$OUTPUT_DIR" > "$BUILD_DIR/diff.txt"; then
    count=$(ls "$OUTPUT_DIR"/*.trace | wc -l)
    echo "PARITY OK: $count traces identical to the frozen golden"
else
    echo "GOLDEN DRIFT:" >&2
    head -80 "$BUILD_DIR/diff.txt" >&2
    exit 1
fi
