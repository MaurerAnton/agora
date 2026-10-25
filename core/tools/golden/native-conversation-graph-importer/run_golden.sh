#!/usr/bin/env bash
# Generates or verifies the golden traces for NativeConversationGraphImporter.kt.
#
# The importer and every module it touches compile VERBATIM from the app tree:
# NativeConversationGraphImporter.kt, NativeConversationGraphSource.kt, NativeGraphJsonValue.kt,
# NativeConversationMediaRestorer.kt, NativeBackupArchive.kt, NativeRunArchivePolicy.kt,
# MessageSource.kt, MaintenanceDebt.kt, SemanticIndexLedger.kt, the two migration planners, the
# ChatDao/entity cluster and the settings/format models. Three boundaries are substituted:
#  1. android.util.JsonReader/JsonWriter/JsonScope/MalformedJsonException + StringPool — vendored
#     verbatim from AOSP by the native-conversation-graph-source stand and compiled with javac
#     (this stand reuses that stand's vendor directory and prepare script);
#  2. DataImporter's pieces — ImportStrategy and the three sanitizeImported* helpers lifted
#     byte-for-byte by this stand's extractor;
#  3. the Android/Room surface — Context/ContentResolver (with the archive's descriptor path),
#     FileProvider, MaintenanceDebtWorker.schedule, and the stand ChatDatabase over the
#     Room-generated schema 36 database (chatDao/semanticIndexDao/maintenanceDebtDao MiniRoom
#     proxies plus Room's BEGIN/COMMIT/ROLLBACK stack).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"
citations_stand="$root/core/tools/golden/citations"
graph_stand="$root/core/tools/golden/native-conversation-graph-source"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx + commons jars)" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
android_classes="$here/build/android-classes"
vendor_src="$here/build/vendor-src"
import_pieces="$here/build/ImportPieces.kt"
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars:$android_classes"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh" "$android_classes"

echo "Generating scenarios..."
python3 "$here/gen_scenarios.py"

echo "Staging the vendored android.util.Json* sources (from the graph-source stand)..."
python3 "$graph_stand/prepare_vendor.py" "$graph_stand/vendor/android-util-json" "$vendor_src"

echo "Compiling the vendored Android JSON platform types..."
javac -nowarn -d "$android_classes" "$vendor_src"/*.java

echo "Extracting ImportStrategy + sanitizeImported* from DataImporter.kt..."
python3 "$here/extract_import_pieces.py" "$app/data/DataImporter.kt" "$import_pieces"

sources=(
    "$import_pieces"
    # ---- settings / format / archive cluster (the native-backup-archive stand's recipe) -----
    "$app/util/Constants.kt"
    "$app/model/OpenAiServiceTiers.kt"
    "$app/data/PromptTemplateItem.kt"
    "$app/data/EmbeddingModelConfig.kt"
    "$app/data/SettingsPreferenceSchema.kt"
    "$app/data/SettingsContracts.kt"
    "$app/data/NativeBackupFormat.kt"
    "$app/data/NativeBackupMediaPolicy.kt"
    "$app/data/NativeBackupArchive.kt"
    # ---- the graph chain (verbatim) ---------------------------------------------------------
    "$app/data/NativeConversationGraphSource.kt"
    "$app/data/NativeGraphJsonValue.kt"
    "$app/data/NativeConversationMediaRestorer.kt"
    "$app/data/NativeConversationGraphImporter.kt"
    # ---- policies, semantics, planners ------------------------------------------------------
    "$app/data/NativeRunArchivePolicy.kt"
    "$app/model/MessageSource.kt"
    "$app/data/local/MaintenanceDebt.kt"
    "$app/data/local/SemanticIndexLedger.kt"
    "$app/data/local/migration/LegacyRunBackfillPlanner.kt"
    "$app/data/local/migration/RegenerationTreeRepairPlanner.kt"
    # ---- ChatDao / entity cluster (the conversation-export-snapshot-reader stand's recipe) ----
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
    # ---- models -------------------------------------------------------------------------------
    "$app/model/RuntimeIdentity.kt"
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
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
    "$app/model/ThinkingControl.kt"
    # ---- automation ---------------------------------------------------------------------------
    "$app/automation/LoopPolicy.kt"
    # ---- leaf stubs from the citations stand ---------------------------------------------------
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
    # ---- stand platform -------------------------------------------------------------------------
    "$here/platform/Room.kt"
    "$here/platform/RoomDatabase.kt"
    "$here/platform/RoomReader.kt"
    "$here/platform/MiniRoom.kt"
    "$here/platform/StandChatDatabase.kt"
    "$here/platform/Android.kt"
    "$here/platform/AndroidNet.kt"
    "$here/platform/AndroidPm.kt"
    "$here/platform/FileProvider.kt"
    "$here/platform/MaintenanceDebtWorker.kt"
    "$here/platform/ParcelFileDescriptor.kt"
    "$here/platform/DebugLog.kt"
    "$here/platform/Preferences.kt"
    "$here/platform/SecretCrypto.kt"
    "$here/platform/SettingsManager.kt"
    "$here/NativeConversationGraphImporterTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classpath="$jars:$android_classes"
echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$classpath" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/native/conversation/graph/importer/golden/NativeConversationGraphImporterTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     native.conversation.graph.importer.golden.NativeConversationGraphImporterTraceDumpKt \
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
