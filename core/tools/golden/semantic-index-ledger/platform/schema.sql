-- Room-generated DDL for ChatDatabase schema 46 (entity createSql + indices), copied
-- verbatim from app/schemas/com.newoether.agora.data.local.ChatDatabase/46.json.

CREATE TABLE IF NOT EXISTS `conversations` (`id` TEXT NOT NULL, `title` TEXT NOT NULL, `lastUpdated` INTEGER NOT NULL, `dataChangedAt` INTEGER NOT NULL, `selectedBranchesJson` TEXT, `systemPromptId` TEXT, `modelId` TEXT, `taskId` TEXT, `origin` TEXT NOT NULL, `graduated` INTEGER NOT NULL, `draftText` TEXT NOT NULL, `draftAttachments` TEXT, `selectedRunBranchesJson` TEXT, `hasUnreadGeneration` INTEGER NOT NULL, `isPinned` INTEGER NOT NULL DEFAULT 0, `todoJson` TEXT, `parentConversationId` TEXT, `createdAt` INTEGER NOT NULL DEFAULT 0, `subagentResultJson` TEXT, `subagentResultId` TEXT, `subagentResultStatus` TEXT, `subagentDeliveredResultId` TEXT, PRIMARY KEY(`id`));
CREATE INDEX IF NOT EXISTS `index_conversations_taskId` ON `conversations` (`taskId`);
CREATE INDEX IF NOT EXISTS `index_conversations_parentConversationId` ON `conversations` (`parentConversationId`);
CREATE INDEX IF NOT EXISTS `index_conversations_modelId` ON `conversations` (`modelId`);

CREATE TABLE IF NOT EXISTS `runs` (`id` TEXT NOT NULL, `conversationId` TEXT NOT NULL, `parentRunId` TEXT, `status` TEXT NOT NULL, `activeSlot` INTEGER, `startedAt` INTEGER NOT NULL, `lastCheckpointAt` INTEGER NOT NULL, `stopRequestedAt` INTEGER, `endedAt` INTEGER, `endReason` TEXT, `currentPass` INTEGER NOT NULL, `legacyAmbiguous` INTEGER NOT NULL, PRIMARY KEY(`id`), FOREIGN KEY(`conversationId`) REFERENCES `conversations`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE , FOREIGN KEY(`parentRunId`) REFERENCES `runs`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE );
CREATE INDEX IF NOT EXISTS `index_runs_conversationId` ON `runs` (`conversationId`);
CREATE INDEX IF NOT EXISTS `index_runs_parentRunId` ON `runs` (`parentRunId`);
CREATE UNIQUE INDEX IF NOT EXISTS `index_runs_conversationId_activeSlot` ON `runs` (`conversationId`, `activeSlot`);

CREATE TABLE IF NOT EXISTS `messages` (`id` TEXT NOT NULL, `conversationId` TEXT NOT NULL, `parentId` TEXT, `text` TEXT NOT NULL, `images` TEXT NOT NULL, `thoughts` TEXT, `thoughtTitle` TEXT, `tokenCount` INTEGER NOT NULL, `inputTokenCount` INTEGER, `cachedInputTokenCount` INTEGER, `cacheWriteInputTokenCount` INTEGER, `uncachedInputTokenCount` INTEGER, `outputTokenCount` INTEGER, `reasoningTokenCount` INTEGER, `generationDurationMs` INTEGER, `status` TEXT NOT NULL, `participant` TEXT NOT NULL, `timestamp` INTEGER NOT NULL, `thoughtTimeMs` INTEGER, `modelName` TEXT, `toolCallJson` TEXT, `attachmentMeta` TEXT, `runId` TEXT NOT NULL, `runSequence` INTEGER NOT NULL, `consumedAtPass` INTEGER, `sourceJson` TEXT, PRIMARY KEY(`id`), FOREIGN KEY(`conversationId`) REFERENCES `conversations`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE , FOREIGN KEY(`runId`) REFERENCES `runs`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE );
CREATE INDEX IF NOT EXISTS `index_messages_conversationId` ON `messages` (`conversationId`);
CREATE INDEX IF NOT EXISTS `index_messages_runId` ON `messages` (`runId`);
CREATE INDEX IF NOT EXISTS `index_messages_runId_runSequence` ON `messages` (`runId`, `runSequence`);
CREATE INDEX IF NOT EXISTS `index_messages_conversationId_id` ON `messages` (`conversationId`, `id`);
CREATE INDEX IF NOT EXISTS `index_messages_modelName_id_conversationId` ON `messages` (`modelName`, `id`, `conversationId`);
CREATE INDEX IF NOT EXISTS `index_messages_parentId` ON `messages` (`parentId`);
CREATE INDEX IF NOT EXISTS `index_messages_id_images_attachmentMeta` ON `messages` (`id`, `images`, `attachmentMeta`);
CREATE INDEX IF NOT EXISTS `index_messages_timestamp_id` ON `messages` (`timestamp`, `id`);

CREATE TABLE IF NOT EXISTS `embeddings` (`id` INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, `messageId` TEXT NOT NULL, `modelId` TEXT NOT NULL, `embedding` BLOB NOT NULL, `chunkText` TEXT NOT NULL, `dimension` INTEGER NOT NULL, `sourceFingerprint` TEXT, `generation` INTEGER NOT NULL DEFAULT 0);
CREATE UNIQUE INDEX IF NOT EXISTS `index_embeddings_messageId_modelId` ON `embeddings` (`messageId`, `modelId`);
CREATE INDEX IF NOT EXISTS `index_embeddings_modelId` ON `embeddings` (`modelId`);

CREATE TABLE IF NOT EXISTS `tasks` (`id` TEXT NOT NULL, `name` TEXT NOT NULL, `prompt` TEXT NOT NULL, `systemPrompt` TEXT, `systemPromptId` TEXT, `modelId` TEXT, `cronExpr` TEXT NOT NULL, `runAt` INTEGER, `nextRunAt` INTEGER NOT NULL, `enabled` INTEGER NOT NULL, `createdAt` INTEGER NOT NULL, `lastRunAt` INTEGER, PRIMARY KEY(`id`));

CREATE TABLE IF NOT EXISTS `loops` (`conversationId` TEXT NOT NULL, `intervalMs` INTEGER NOT NULL, `prompt` TEXT, `nextFireAt` INTEGER NOT NULL, `cycleCount` INTEGER NOT NULL, `maxCycles` INTEGER, `active` INTEGER NOT NULL, `revision` INTEGER NOT NULL, PRIMARY KEY(`conversationId`), FOREIGN KEY(`conversationId`) REFERENCES `conversations`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE );

CREATE TABLE IF NOT EXISTS `new_chat_persist` (`id` INTEGER NOT NULL, `modelId` TEXT, `systemPromptId` TEXT, `conversationSettingsJson` TEXT, `draftText` TEXT NOT NULL, `draftAttachments` TEXT, PRIMARY KEY(`id`));

CREATE TABLE IF NOT EXISTS `conversation_settings_transfer` (`conversationId` TEXT NOT NULL, `settingsJson` TEXT, PRIMARY KEY(`conversationId`), FOREIGN KEY(`conversationId`) REFERENCES `conversations`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE );

CREATE TABLE IF NOT EXISTS `conversation_settings_import_transfer` (`id` INTEGER NOT NULL, `transferId` TEXT NOT NULL, `settingsJson` TEXT NOT NULL, `mode` TEXT NOT NULL, PRIMARY KEY(`id`));

CREATE TABLE IF NOT EXISTS `maintenance_debt` (`kind` TEXT NOT NULL, `identity` TEXT NOT NULL, `state` TEXT NOT NULL, `revision` INTEGER NOT NULL, `updatedAt` INTEGER NOT NULL, `claimId` TEXT, `claimedAt` INTEGER, PRIMARY KEY(`kind`, `identity`));
CREATE INDEX IF NOT EXISTS `index_maintenance_debt_state_updatedAt_kind_identity` ON `maintenance_debt` (`state`, `updatedAt`, `kind`, `identity`);

CREATE TABLE IF NOT EXISTS `semantic_index_ledger` (`modelId` TEXT NOT NULL, `state` TEXT NOT NULL, `sourceRevision` INTEGER NOT NULL, `completedRevision` INTEGER NOT NULL, `reconcileRevision` INTEGER NOT NULL, `updatedAt` INTEGER NOT NULL, `generation` INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(`modelId`));

CREATE TABLE IF NOT EXISTS `semantic_index_work` (`modelId` TEXT NOT NULL, `messageId` TEXT NOT NULL, `sourceFingerprint` TEXT, `sourceRevision` INTEGER NOT NULL, `updatedAt` INTEGER NOT NULL, PRIMARY KEY(`modelId`, `messageId`), FOREIGN KEY(`modelId`) REFERENCES `semantic_index_ledger`(`modelId`) ON UPDATE NO ACTION ON DELETE CASCADE );
CREATE INDEX IF NOT EXISTS `index_semantic_index_work_modelId_sourceRevision_messageId` ON `semantic_index_work` (`modelId`, `sourceRevision`, `messageId`);

CREATE TABLE IF NOT EXISTS `semantic_sources` (`messageId` TEXT NOT NULL, `conversationId` TEXT NOT NULL, `eligible` INTEGER NOT NULL, `sourcePrefix` BLOB NOT NULL, `sourceFingerprint` TEXT, `searchLength` INTEGER NOT NULL, `historyEligible` INTEGER NOT NULL, PRIMARY KEY(`messageId`), FOREIGN KEY(`conversationId`) REFERENCES `conversations`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE );
CREATE INDEX IF NOT EXISTS `index_semantic_sources_conversationId_messageId` ON `semantic_sources` (`conversationId`, `messageId`);

CREATE TABLE IF NOT EXISTS `semantic_conversations` (`conversationId` TEXT NOT NULL, `sourceRevision` INTEGER NOT NULL, `eligibleCount` INTEGER NOT NULL, `cacheEligible` INTEGER NOT NULL, PRIMARY KEY(`conversationId`), FOREIGN KEY(`conversationId`) REFERENCES `conversations`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE );

CREATE TABLE IF NOT EXISTS `semantic_model_conversations` (`modelId` TEXT NOT NULL, `conversationId` TEXT NOT NULL, `cachedCount` INTEGER NOT NULL, `indexedRevision` INTEGER NOT NULL, PRIMARY KEY(`modelId`, `conversationId`), FOREIGN KEY(`conversationId`) REFERENCES `conversations`(`id`) ON UPDATE NO ACTION ON DELETE CASCADE );
CREATE INDEX IF NOT EXISTS `index_semantic_model_conversations_conversationId` ON `semantic_model_conversations` (`conversationId`);

CREATE TABLE IF NOT EXISTS `semantic_metadata_state` (`id` INTEGER NOT NULL, `complete` INTEGER NOT NULL, `afterMessageId` TEXT, PRIMARY KEY(`id`));

CREATE TABLE IF NOT EXISTS `attachment_reference_owners` (`kind` INTEGER NOT NULL, `ownerId` TEXT NOT NULL, `token` TEXT NOT NULL, `dirty` INTEGER NOT NULL, `invalid` INTEGER NOT NULL, PRIMARY KEY(`kind`, `ownerId`));
CREATE INDEX IF NOT EXISTS `index_attachment_reference_owners_dirty_kind_ownerId` ON `attachment_reference_owners` (`dirty`, `kind`, `ownerId`);
CREATE INDEX IF NOT EXISTS `index_attachment_reference_owners_invalid_kind_ownerId` ON `attachment_reference_owners` (`invalid`, `kind`, `ownerId`);

CREATE TABLE IF NOT EXISTS `attachment_references` (`kind` INTEGER NOT NULL, `ownerId` TEXT NOT NULL, `path` TEXT NOT NULL, PRIMARY KEY(`kind`, `ownerId`, `path`), FOREIGN KEY(`kind`, `ownerId`) REFERENCES `attachment_reference_owners`(`kind`, `ownerId`) ON UPDATE NO ACTION ON DELETE CASCADE );
CREATE INDEX IF NOT EXISTS `index_attachment_references_path` ON `attachment_references` (`path`);

CREATE TABLE IF NOT EXISTS `attachment_reference_state` (`id` INTEGER NOT NULL, `phase` INTEGER NOT NULL, `afterId` TEXT, `revision` INTEGER NOT NULL, PRIMARY KEY(`id`));
