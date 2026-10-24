#!/usr/bin/env bash
# Generates or verifies the golden traces for data/DataExporter.kt.
#
# DataExporter.kt and every module it calls compile VERBATIM from the app tree:
# ConversationExportSnapshotReader.kt, ExportSpool.kt, NativeBackupV5Writer.kt,
# NativeBackupV5Baseline.kt, NativeBackupMediaPolicy.kt, NativeBackupSecretsPolicy.kt,
# PortableSettingsArchive.kt, MemoryManager.kt, SkillManager.kt and their models. The stand
# substitutes only the Android platform surface (Context dirs, ContentResolver, PackageManager,
# Uri), the DataStore SettingsManager double, and Apache Commons Compress (real jar). The
# identity policy inside CustomProviderIdentity.kt is lifted byte-for-byte by the
# provider-identity stand's extractor; sanitizeImportedLoop is lifted byte-for-byte from
# DataImporter.kt by this stand's extractor (both generated into build/).
#
# Usage:
#   KOTLINX_JARS=... COROUTINES_JAR=... ./run_golden.sh            # verify
#   KOTLINX_JARS=... COROUTINES_JAR=... ./run_golden.sh --freeze   # regenerate traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"
citations_stand="$root/core/tools/golden/citations"
identity_stand="$root/core/tools/golden/provider-identity"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
kotlinc_lib="$kotlinc_dir/../lib"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"
coroutines="${COROUTINES_JAR:-}"
if [[ -z "$jars" || -z "$coroutines" ]]; then
    echo "error: KOTLINX_JARS and COROUTINES_JAR must point at the kotlinx jars" >&2
    exit 2
fi
runtime="$kotlinc_lib/kotlin-stdlib.jar:$kotlinc_lib/kotlin-reflect.jar:$jars:$coroutines"
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
identity="$here/build/IdentityPolicy.kt"
sanitize="$here/build/SanitizeImportedLoop.kt"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting the identity policy and sanitizeImportedLoop..."
python3 "$identity_stand/extract_identity.py" "$app/data/CustomProviderIdentity.kt" "$identity"
python3 "$here/extract_sanitize_imported_loop.py" "$app/data/DataImporter.kt" "$sanitize"

sources=(
    "$app/data/DataExporter.kt"
    "$app/data/ConversationExportSnapshotReader.kt"
    "$app/data/ExportSpool.kt"
    "$app/data/NativeBackupV5Writer.kt"
    "$app/data/NativeBackupV5Baseline.kt"
    "$app/data/NativeBackupMediaPolicy.kt"
    "$app/data/NativeBackupSecretsPolicy.kt"
    "$app/data/PortableSettingsArchive.kt"
    "$app/data/local/ChatDao.kt"
    "$app/data/local/ChatDaoRuntimeRecovery.kt"
    "$app/data/local/ChatContextCompactDao.kt"
    "$app/data/local/ChatAutomationDao.kt"
    "$app/data/local/ChatProviderContextDao.kt"
    "$app/data/local/ChatSearchDao.kt"
    "$app/data/local/ChatMediaReferencesDao.kt"
    "$app/data/local/NewChatPersistDao.kt"
    "$app/data/local/ChatEntities.kt"
    "$app/data/local/RunEntity.kt"
    "$app/data/local/MessageContextTopology.kt"
    "$app/data/local/RunBranchSelectionIntegrity.kt"
    "$app/data/local/ForkEmbeddingClonePolicy.kt"
    "$app/data/local/EmbeddingSearchRow.kt"
    "$app/model/RuntimeIdentity.kt"
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ModelId.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/Citation.kt"
    "$app/model/MessagePersistenceGuard.kt"
    "$app/model/ConversationRuntimeReducer.kt"
    "$app/model/ConversationCommand.kt"
    "$app/model/RuntimeTransition.kt"
    "$app/model/RunRecoveryPolicy.kt"
    "$app/model/RunState.kt"
    "$app/model/RunEffect.kt"
    "$app/model/OpenAiServiceTiers.kt"
    "$app/model/ThinkingControl.kt"
    "$app/util/Constants.kt"
    "$app/automation/LoopPolicy.kt"
    "$app/data/PromptTemplateItem.kt"
    "$app/data/EmbeddingModelConfig.kt"
    "$app/data/SettingsPreferenceSchema.kt"
    "$app/data/SettingsDefaults.kt"
    "$app/data/DefaultSystemPrompt.kt"
    "$app/data/SettingsContracts.kt"
    "$app/data/NativeBackupFormat.kt"
    "$app/data/CustomProviderConfig.kt"
    "$app/data/CustomProviderNamePolicy.kt"
    "$app/data/MemoryManager.kt"
    "$app/data/SkillManager.kt"
    "$app/data/DescriptionMetadataStore.kt"
    "$identity"
    "$sanitize"
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/platform/Room.kt"
    "$here/platform/RoomDatabase.kt"
    "$here/platform/RoomReader.kt"
    "$here/platform/MiniRoom.kt"
    "$here/platform/StandChatDatabase.kt"
    "$here/platform/Android.kt"
    "$here/platform/AndroidNet.kt"
    "$here/platform/AndroidPm.kt"
    "$here/platform/AndroidProcess.kt"
    "$here/platform/DebugLog.kt"
    "$here/platform/Preferences.kt"
    "$here/platform/SecretCrypto.kt"
    "$here/platform/SettingsManager.kt"
    "$here/DataExporterTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Generating scenarios..."
python3 "$here/gen_scenarios.py"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$runtime" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/data/exporter/golden/DataExporterTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     data.exporter.golden.DataExporterTraceDumpKt \
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
