// Ports of data/local/migration/Migration20To21.kt .. Migration34To35.kt.
//
// Every migration here is SQL-only: the port emits the same statements in the same order with no
// bind arguments. SQL text and whitespace are part of the Room migration contract and are pinned
// by the frozen golden traces.
#include "agora/core/migration_20_35.hpp"

#include <string>
#include <string_view>

namespace agora::core::migration_20_35 {
namespace {

constexpr std::string_view kTokenUsageColumns[] = {
    "inputTokenCount",
    "cachedInputTokenCount",
    "uncachedInputTokenCount",
    "outputTokenCount",
    "reasoningTokenCount",
};

constexpr char kNewChatPersistTable[] = R"(CREATE TABLE IF NOT EXISTS new_chat_persist (
    id INTEGER NOT NULL,
    modelId TEXT,
    systemPromptId TEXT,
    conversationSettingsJson TEXT,
    draftText TEXT NOT NULL,
    draftAttachments TEXT,
    PRIMARY KEY(id)
))";

constexpr char kConversationSettingsTransferTable[] =
    R"(CREATE TABLE IF NOT EXISTS conversation_settings_transfer (
    conversationId TEXT NOT NULL,
    settingsJson TEXT,
    PRIMARY KEY(conversationId),
    FOREIGN KEY(conversationId) REFERENCES conversations(id)
        ON UPDATE NO ACTION ON DELETE CASCADE
))";

constexpr char kConversationSettingsImportTransferTable[] =
    R"(CREATE TABLE IF NOT EXISTS conversation_settings_import_transfer (
    id INTEGER NOT NULL,
    transferId TEXT NOT NULL,
    settingsJson TEXT NOT NULL,
    mode TEXT NOT NULL,
    PRIMARY KEY(id)
))";

constexpr char kMaintenanceDebtTable[] = R"(CREATE TABLE IF NOT EXISTS maintenance_debt (
    kind TEXT NOT NULL,
    identity TEXT NOT NULL,
    state TEXT NOT NULL,
    revision INTEGER NOT NULL,
    updatedAt INTEGER NOT NULL,
    claimId TEXT,
    claimedAt INTEGER,
    PRIMARY KEY(kind, identity)
))";

constexpr char kMaintenanceDebtIndex[] =
    R"(CREATE INDEX IF NOT EXISTS index_maintenance_debt_state_updatedAt_kind_identity
ON maintenance_debt (state, updatedAt, kind, identity))";

constexpr std::string_view kMaintenanceKinds[] = {
    "ATTACHMENT_ORPHANS",
    "EMBEDDING_ORPHANS",
    "RUN_BRANCHES",
};

constexpr char kSemanticIndexLedgerTable[] = R"(CREATE TABLE IF NOT EXISTS semantic_index_ledger (
    modelId TEXT NOT NULL,
    state TEXT NOT NULL,
    sourceRevision INTEGER NOT NULL,
    completedRevision INTEGER NOT NULL,
    updatedAt INTEGER NOT NULL,
    PRIMARY KEY(modelId)
))";

constexpr char kSemanticIndexWorkTable[] = R"(CREATE TABLE IF NOT EXISTS semantic_index_work (
    modelId TEXT NOT NULL,
    messageId TEXT NOT NULL,
    sourceFingerprint TEXT,
    sourceRevision INTEGER NOT NULL,
    updatedAt INTEGER NOT NULL,
    PRIMARY KEY(modelId, messageId),
    FOREIGN KEY(modelId) REFERENCES semantic_index_ledger(modelId)
        ON UPDATE NO ACTION ON DELETE CASCADE
))";

constexpr char kSemanticIndexWorkIndex[] =
    R"(CREATE INDEX IF NOT EXISTS index_semantic_index_work_modelId_sourceRevision_messageId
ON semantic_index_work (modelId, sourceRevision, messageId))";

constexpr char kReconcileRevisionUpdate[] = R"(UPDATE semantic_index_ledger
SET reconcileRevision = sourceRevision
WHERE state = 'NEEDS_RECONCILE')";

}  // namespace

void migrate_20_21(Database& database) {
    for (std::string_view column : kTokenUsageColumns) {
        std::string sql = "ALTER TABLE messages ADD COLUMN ";
        sql += column;
        sql += " INTEGER";
        database.exec_sql(sql, {});
    }
}

void migrate_21_22(Database& database) {
    database.exec_sql(
        "ALTER TABLE conversations ADD COLUMN hasUnreadGeneration INTEGER NOT NULL DEFAULT 0", {});
}

void migrate_22_23(Database& database) {
    database.exec_sql(
        "CREATE INDEX IF NOT EXISTS index_embeddings_modelId ON embeddings (modelId)", {});
}

void migrate_23_24(Database& database) {
    database.exec_sql(kNewChatPersistTable, {});
}

void migrate_24_25(Database& database) {
    database.exec_sql(kConversationSettingsTransferTable, {});
}

void migrate_25_26(Database& database) {
    database.exec_sql("ALTER TABLE messages ADD COLUMN cacheWriteInputTokenCount INTEGER", {});
}

void migrate_26_27(Database& database) {
    database.exec_sql(kConversationSettingsImportTransferTable, {});
}

void migrate_27_28(Database& database) {
    database.exec_sql(kMaintenanceDebtTable, {});
    database.exec_sql(kMaintenanceDebtIndex, {});
    for (std::string_view kind : kMaintenanceKinds) {
        std::string sql =
            "INSERT OR IGNORE INTO maintenance_debt (\n"
            "    kind, identity, state, revision, updatedAt, claimId, claimedAt\n"
            ") VALUES ('";
        sql += kind;
        sql += "', '*', 'PENDING', 1, 0, NULL, NULL)";
        database.exec_sql(sql, {});
    }
}

void migrate_28_29(Database& database) {
    database.exec_sql(kSemanticIndexLedgerTable, {});
    database.exec_sql(kSemanticIndexWorkTable, {});
    database.exec_sql(kSemanticIndexWorkIndex, {});
}

void migrate_29_30(Database& database) {
    database.exec_sql(
        "ALTER TABLE semantic_index_ledger "
        "ADD COLUMN reconcileRevision INTEGER NOT NULL DEFAULT 0",
        {});
    database.exec_sql(kReconcileRevisionUpdate, {});
    database.exec_sql("ALTER TABLE embeddings ADD COLUMN sourceFingerprint TEXT", {});
}

void migrate_30_31(Database& database) {
    database.exec_sql(
        "CREATE INDEX index_messages_id_images_attachmentMeta ON messages(id, images, "
        "attachmentMeta)",
        {});
}

void migrate_31_32(Database& database) {
    database.exec_sql("ALTER TABLE messages ADD COLUMN generationDurationMs INTEGER", {});
}

void migrate_32_33(Database& database) {
    database.exec_sql(
        "ALTER TABLE conversations ADD COLUMN dataChangedAt INTEGER NOT NULL DEFAULT 0", {});
}

void migrate_33_34(Database& database) {
    database.exec_sql("ALTER TABLE tasks ADD COLUMN systemPromptId TEXT", {});
}

void migrate_34_35(Database& database) {
    database.exec_sql("ALTER TABLE messages ADD COLUMN sourceJson TEXT", {});
}

bool migrate(int from_version, Database& database) {
    switch (from_version) {
        case 20: migrate_20_21(database); return true;
        case 21: migrate_21_22(database); return true;
        case 22: migrate_22_23(database); return true;
        case 23: migrate_23_24(database); return true;
        case 24: migrate_24_25(database); return true;
        case 25: migrate_25_26(database); return true;
        case 26: migrate_26_27(database); return true;
        case 27: migrate_27_28(database); return true;
        case 28: migrate_28_29(database); return true;
        case 29: migrate_29_30(database); return true;
        case 30: migrate_30_31(database); return true;
        case 31: migrate_31_32(database); return true;
        case 32: migrate_32_33(database); return true;
        case 33: migrate_33_34(database); return true;
        case 34: migrate_34_35(database); return true;
        default: return false;
    }
}

}  // namespace agora::core::migration_20_35
