#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/android_json.hpp"
#include "agora/core/chat_dao.hpp"
#include "agora/core/export_extra_settings.hpp"
#include "agora/core/maintenance_debt.hpp"
#include "agora/core/native_conversation_graph_source.hpp"
#include "agora/core/semantic_index_ledger.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::native_graph_import {

/**
 * Port of data/NativeConversationGraphImporter.kt — the three public phases (count, header read,
 * transactional import) over the chained conversation graph.
 *
 * Composed from the pinned ports: chat_dao (every graph write), android_json + the graph source
 * (streaming reads), native_run_archive (terminalize/parent order/ownership), legacy_run_backfill
 * and regeneration_tree_repair (the two run-planning branches), backup_media_policy (draft,
 * attachment and tool-image restore), message_source, semantic_index_ledger and maintenance_debt.
 *
 * The stand's frozen traces pin:
 *  - sanitize order for tasks/conversations/loops and the strategy-dependent id sets (MERGE keeps
 *    dangling tasks, REPLACE clears them and drops unread state);
 *  - both planning branches: the legacy backfill (planner-made run ids, re-encoded run-branch
 *    selections) and the repair branch (inferred parents with legacyAmbiguous, deleted duplicate
 *    leading inputs, reparenting, resequencing) plus the recovered-run status rewrite;
 *  - MERGE upsert-vs-skip rules, the getRun insert guard, the 64-row message batching and its
 *    images override, REPLACE wipes with cascades, image restore for archive v>=4 and the legacy
 *    images/videos-by-message maps for v<4;
 *  - the transaction's side effects: settings import transfer (kotlinx map encoding with the
 *    "null" key), the maintenance enqueues (reconcile kinds in declaration order), the semantic
 *    mutation and scheduleMaintenance;
 *  - kotlinx' decode failures for the injected `Json { ignoreUnknownKeys, coerceInputValues }`
 *    config: MissingFieldException carries NO path suffix here (decodeFromJsonElement has no
 *    source), while unknown keys are ignored.
 *
 * The driver masks generated UUIDs as <uuid:N> (first appearance) and 13-digit epoch values as
 * <clock>; new_uuid/now_millis are callbacks so the C++ side mirrors the generation order the
 * mask depends on.
 */
using Exception = android_json::Exception;

enum class ImportStrategy { Merge, Replace, Skip };

/** MaintenanceDebtEntity's Kotlin companion: the reconcile enqueues after every import. */
inline constexpr std::string_view kReconcileIdentity = "*";
inline constexpr std::string_view kReconcileKinds[] = {"ATTACHMENT_ORPHANS", "EMBEDDING_ORPHANS",
                                                       "RUN_BRANCHES"};

struct GraphCounts {
    int conversations = 0;
    int tasks = 0;
    int loops = 0;
};

/** NativeConversationGraphImporter.ConversationGraphHeaders. */
struct ConversationGraphHeaders {
    std::vector<::agora::core::automation::TaskEntity> tasks;
    std::vector<::agora::core::ChatEntity> conversations;
    std::vector<::agora::core::context_compact::RunEntity> runs;
    bool source_run_ids_were_unique = true;
    std::vector<::agora::core::automation::LoopEntity> loops;
    /** LinkedHashSet order: MERGE first holds the existing database ids, then the graph's. */
    std::vector<std::string> available_conversation_ids;
    std::vector<std::pair<std::string, export_extra_settings::ConversationSettings>>
        conversation_settings;
};

/** NativeConversationMediaRestorer.RestoredMedia (scenario-seeded; the restore path is not run). */
struct RestoredMediaFile {
    std::string absolute_path;
    std::string uri;
};

struct RestoredMedia {
    std::map<std::string, RestoredMediaFile> archive_files;
    std::map<std::string, std::map<int, std::string>> legacy_images_by_message;
    std::map<std::string, std::map<int, std::string>> legacy_videos_by_message;
};

/**
 * The values the real call site injects: DataImporter's resolveSystemPromptId (null-preserving,
 * containsKey semantics), UUID.randomUUID for the settings-transfer id, System.currentTimeMillis,
 * and MaintenanceDebtWorker.schedule.
 */
struct ImportCallbacks {
    std::function<std::optional<std::string>(const std::optional<std::string>&)>
        resolve_system_prompt_id;
    std::function<std::string()> new_uuid;
    std::function<std::int64_t()> now_millis;
    std::function<void()> schedule_maintenance;
};

/**
 * Adapts the graph source's handed-out stream to the android_json Reader source: Kotlin wraps
 * `graphSource.open()`'s InputStream in JsonReader, so every phase reads through this bridge.
 * Closing is the graph source's job (close() reaches the state either way).
 */
class GraphStreamAdapter final : public android_json::Source {
public:
    explicit GraphStreamAdapter(native_graph::NativeConversationGraphSource::Stream& stream)
        : stream_(stream) {}

    long long read(char* buffer, long long capacity) override {
        return stream_.read(buffer, capacity);
    }

private:
    native_graph::NativeConversationGraphSource::Stream& stream_;
};

/**
 * NativeConversationGraphImporter. The DAO/semantic/debt surfaces are the pinned ports over the
 * caller's SQL executor; the caller owns the transaction boundary exactly like Room does (the
 * import wraps its writes in one sql.begin()/commit()/rollback() — withSemanticGraphMutation's
 * single RoomDatabase level).
 */
class NativeConversationGraphImporter {
public:
    NativeConversationGraphImporter(::agora::core::chat_dao::ChatDao& dao,
                                    ::agora::core::semantic_index::SemanticIndexDao& semantic,
                                    ::agora::core::maintenance_debt::MaintenanceDebtDao& debt,
                                    ::agora::core::SqlExecutor& sql, ImportCallbacks callbacks);

    GraphCounts count(android_json::Source& stream);

    ConversationGraphHeaders read_headers(android_json::Source& stream, ImportStrategy strategy,
                                          const RestoredMedia& restored_media);

    /** Returns the settings-transfer id (java UUID.randomUUID()). */
    std::string import_graph(native_graph::NativeConversationGraphSource& source,
                             ImportStrategy strategy, const ConversationGraphHeaders& headers,
                             const RestoredMedia& restored_media, int archive_version,
                             const ::agora::core::semantic_index::SemanticModelSnapshot& snapshot);

private:
    ::agora::core::chat_dao::ChatDao& dao_;
    ::agora::core::semantic_index::SemanticIndexDao& semantic_;
    ::agora::core::maintenance_debt::MaintenanceDebtDao& debt_;
    ::agora::core::SqlExecutor& sql_;
    ImportCallbacks callbacks_;
};

}  // namespace agora::core::native_graph_import
