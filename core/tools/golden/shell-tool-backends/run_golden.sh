#!/usr/bin/env bash
# Generates or verifies the golden traces for tool/ShellToolBackends.kt.
#
# ShellToolBackends.kt compiles VERBATIM from the app tree over the pinned leaf modules
# (ShellClient/ConchSseParser/ShellToolJson/SshClient/ShellCrypto/ConchHandshake/
# PortableGlobMatcher/Constants/SandboxManager interface + the ToolProvider contracts and the
# ChatMessage model chain). Platform substitutes (see README):
#   1. platform/HttpStream.kt     — the api/HttpClient.kt streaming surface (streamPost/
#      StreamHandle) with a recorded invocation log and SSE-line sealing on X-Encryption;
#   2. platform/ConchNetwork.kt   — the GET/POST transport (shared shape with the shell-client
#      stand) plus streamCalls/LINE_LIMIT/CONTROL_LIMIT;
#   3. platform/JSch.kt           — the JSch subset SshClient.kt uses, with a full event log
#      (StrictHostKeyChecking decisions run SshClient's REAL HostKeyRepository);
#   4. platform/ScriptedSandboxManager.kt — scripted SandboxManager;
#   5. platform/{DebugLog,Base64,GenerationContext,ShellDeviceConfig,Immutable,MarkdownState}.
#
# Usage:
#   JAVA_HOME=<jdk17> KOTLINC=<kotlinc> KOTLINX_JARS=<kotlinx jars> ./run_golden.sh            # verify
#   JAVA_HOME=<jdk17> KOTLINC=<kotlinc> KOTLINX_JARS=<kotlinx jars> ./run_golden.sh --freeze   # regenerate
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

# The backends suspend: locate the coroutines jar next to the serialization jars by default.
coroutines="${KOTLINX_COROUTINES_JAR:-}"
if [[ -z "$coroutines" ]]; then
    first="${jars%%:*}"
    coroutines="$(ls "$(dirname "$first")"/kotlinx-coroutines-core-jvm-*.jar 2>/dev/null | head -1 || true)"
fi
if [[ ! -f "$coroutines" ]]; then
    echo "error: kotlinx-coroutines-core-jvm jar not found (set KOTLINX_COROUTINES_JAR)" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
android_classes="$here/build/android-classes"
if [[ -n "${JAVA_HOME:-}" ]]; then
    java_bin="$JAVA_HOME/bin/java"
    javac_bin="$JAVA_HOME/bin/javac"
else
    java_bin="java"
    javac_bin="javac"
fi
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars:$coroutines:$android_classes"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh" "$android_classes"

echo "Generating scenarios..."
python3 "$here/gen_scenarios.py"

echo "Compiling the vendored Android Base64 platform type..."
"$javac_bin" -nowarn -d "$android_classes" "$here/platform/Base64.java"

sources=(
    # ChatMessage model chain + ToolDefinition wire models (proven by the shell-definitions stand)
    "$app/api/ProviderRequestModels.kt"
    "$app/api/ToolParametersSerializer.kt"
    "$app/model/ModelId.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/ChatMessage.kt"
    # Pinned leaf modules
    "$app/util/Constants.kt"
    "$app/util/PortableGlobMatcher.kt"
    "$app/util/ShellCrypto.kt"
    "$app/util/ConchHandshake.kt"
    "$app/util/ShellClient.kt"
    "$app/util/SshClient.kt"
    "$app/tool/ConchSseParser.kt"
    "$app/tool/ShellToolJson.kt"
    "$app/tool/ToolProvider.kt"
    "$app/sandbox/SandboxManager.kt"
    # The module under test
    "$app/tool/ShellToolBackends.kt"
    # Stand-local doubles
    "$here/platform/Immutable.kt"
    "$here/platform/MarkdownState.kt"
    "$here/platform/GenerationContext.kt"
    "$here/platform/ShellDeviceConfig.kt"
    "$here/platform/DebugLog.kt"
    "$here/platform/ConchNetwork.kt"
    "$here/platform/HttpStream.kt"
    "$here/platform/JSch.kt"
    "$here/platform/ScriptedSandboxManager.kt"
    "$here/ShellToolBackendsTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classpath="$jars:$coroutines:$android_classes"
echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$classpath" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/shell/tool/backends/golden/ShellToolBackendsTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
"$java_bin" -cp "$classes:$runtime" \
     shell.tool.backends.golden.ShellToolBackendsTraceDumpKt \
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
