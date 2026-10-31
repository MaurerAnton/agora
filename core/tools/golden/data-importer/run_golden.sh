#!/usr/bin/env bash
# Generates or verifies the golden traces for data/DataImporter.kt.
#
# DataImporter.kt and every module it calls compile VERBATIM from the app tree: the graph chain
# (source/importer/media restorer), NativeBackupArchive, PortableSettingsArchive,
# ExportExtraSettings, NativeBackupSecretsPolicy, MemoryManager/SkillManager, FontUtils,
# ConversationSettingsTransferCoordinator/ConversationSettingsState and the ChatDao/entity
# cluster. Four boundaries are substituted:
#  1. android.util.JsonReader/JsonWriter/JsonScope/MalformedJsonException + StringPool — vendored
#     verbatim from AOSP by the native-conversation-graph-source stand and compiled with javac;
#  2. DataExporter.ExportCategory — lifted byte-for-byte by agora_extract export-category (the enum
#     is DataImporter's decision-map key; the export root is not pulled in);
#  3. the Android/Room surface — Context/ContentResolver (with the archive descriptor path),
#     ParcelFileDescriptor, FileProvider, MaintenanceDebtWorker.schedule, and the stand
#     ChatDatabase over the Room-generated schema 36 database;
#  4. the DataStore storage classes — SettingsManager (the data-exporter stand's recording double,
#     extended with the read-back state the importer observes), SettingsRepository (the real
#     ConversationSettingsState.applyImport over a recording persistence boundary) and
#     ConversationRepository (verbatim ChatDao delegation).
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
identity_stand="$root/core/tools/golden/provider-identity"

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
export_category="$here/build/ExportCategory.kt"
identity="$here/build/IdentityPolicy.kt"
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars:$android_classes"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh" "$android_classes"

echo "Generating scenarios..."
"${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" data-importer "$here"

echo "Staging the vendored android.util.Json* sources (from the graph-source stand)..."
"${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" prepare-vendor "$graph_stand/vendor/android-util-json" "$vendor_src"

echo "Compiling the vendored Android JSON platform types..."
javac -nowarn -d "$android_classes" "$vendor_src"/*.java

echo "Extracting DataExporter.ExportCategory from DataExporter.kt..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" export-category "$app/data/DataExporter.kt" "$export_category"

echo "Extracting the identity policy from CustomProviderIdentity.kt..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" provider-identity "$app/data/CustomProviderIdentity.kt" "$identity"

sources=(
    "$export_category"
    "$identity"
    # ---- settings / format / archive cluster ------------------------------------------------
    "$app/util/Constants.kt"
    "$app/model/OpenAiServiceTiers.kt"
    "$app/model/ThinkingControl.kt"
    "$app/data/PromptTemplateItem.kt"
    "$app/data/EmbeddingModelConfig.kt"
    "$app/data/SettingsPreferenceSchema.kt"
    "$app/data/SettingsDefaults.kt"
    "$app/data/DefaultSystemPrompt.kt"
    "$app/data/SettingsContracts.kt"
    "$app/data/NativeBackupFormat.kt"
    "$app/data/NativeBackupMediaPolicy.kt"
    "$app/data/NativeBackupArchive.kt"
    "$app/data/PortableSettingsArchive.kt"
    "$app/data/ExportExtraSettings.kt"
    "$app/data/NativeBackupSecretsPolicy.kt"
    "$app/data/CustomProviderConfig.kt"
    "$app/data/CustomProviderNamePolicy.kt"
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
    # ---- ChatDao / entity cluster -----------------------------------------------------------
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
    # ---- models -----------------------------------------------------------------------------
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
    # ---- automation -------------------------------------------------------------------------
    "$app/automation/LoopPolicy.kt"
    # ---- memories / skills / import root ----------------------------------------------------
    "$app/data/DescriptionMetadataStore.kt"
    "$app/data/MemoryManager.kt"
    "$app/data/SkillManager.kt"
    "$app/data/EmbeddingCacheLocks.kt"
    "$app/data/repository/ConversationSettingsState.kt"
    "$app/data/repository/ConversationSettingsTransferCoordinator.kt"
    "$app/util/FontUtils.kt"
    "$app/data/DataImporter.kt"
    # ---- leaf stubs from the citations stand -------------------------------------------------
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
    # ---- stand platform ----------------------------------------------------------------------
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
    "$here/platform/ConversationRepository.kt"
    "$here/platform/SettingsRepository.kt"
    "$here/DataImporterTraceDump.kt"
    "$here/DataImporterSettingsSnapshot.kt"
    "$here/DataImporterSettingsView.kt"
    "$here/DataImporterTableDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classpath="$jars:$android_classes"
echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$classpath" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/data/importer/golden/DataImporterTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     data.importer.golden.DataImporterTraceDumpKt \
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
