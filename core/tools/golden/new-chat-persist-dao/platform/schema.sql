-- Schema for the chat-media-references-dao stand (data/local/ChatMediaReferencesDao.kt).
--
-- `conversations`, `runs` and `messages` reproduce the Room-generated DDL for ChatEntity,
-- RunEntity and MessageEntity; `new_chat_persist` is the NewChatPersistEntity singleton. The DAO
-- reads only `messages.id/images/attachmentMeta/toolCallJson/conversationId` and the two draft
-- columns, but the tables keep their full declared shape so the projections run against the same
-- physical schema as the app. `runs` exists because `messages.runId` references it and foreign
-- keys are ON; `embeddings` is not reachable from this DAO and is omitted.
CREATE TABLE IF NOT EXISTS conversations (
    id TEXT NOT NULL,
    title TEXT NOT NULL,
    lastUpdated INTEGER NOT NULL,
    dataChangedAt INTEGER NOT NULL,
    selectedBranchesJson TEXT,
    systemPromptId TEXT,
    modelId TEXT,
    taskId TEXT,
    origin TEXT NOT NULL,
    graduated INTEGER NOT NULL,
    draftText TEXT NOT NULL,
    draftAttachments TEXT,
    selectedRunBranchesJson TEXT,
    hasUnreadGeneration INTEGER NOT NULL,
    isPinned INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (id)
);

CREATE INDEX IF NOT EXISTS index_conversations_taskId ON conversations (taskId);

CREATE TABLE IF NOT EXISTS runs (
    id TEXT PRIMARY KEY NOT NULL,
    conversationId TEXT NOT NULL,
    parentRunId TEXT,
    status TEXT NOT NULL,
    activeSlot INTEGER,
    startedAt INTEGER NOT NULL,
    lastCheckpointAt INTEGER NOT NULL,
    stopRequestedAt INTEGER,
    endedAt INTEGER,
    endReason TEXT,
    currentPass INTEGER NOT NULL,
    legacyAmbiguous INTEGER NOT NULL,
    FOREIGN KEY(conversationId) REFERENCES conversations(id) ON UPDATE NO ACTION ON DELETE CASCADE,
    FOREIGN KEY(parentRunId) REFERENCES runs(id) ON UPDATE NO ACTION ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS index_runs_conversationId ON runs (conversationId);
CREATE INDEX IF NOT EXISTS index_runs_parentRunId ON runs (parentRunId);
CREATE UNIQUE INDEX IF NOT EXISTS index_runs_conversationId_activeSlot
    ON runs (conversationId, activeSlot);

CREATE TABLE IF NOT EXISTS messages (
    id TEXT NOT NULL,
    conversationId TEXT NOT NULL,
    parentId TEXT,
    text TEXT NOT NULL,
    images TEXT NOT NULL,
    thoughts TEXT,
    thoughtTitle TEXT,
    tokenCount INTEGER NOT NULL,
    inputTokenCount INTEGER,
    cachedInputTokenCount INTEGER,
    cacheWriteInputTokenCount INTEGER,
    uncachedInputTokenCount INTEGER,
    outputTokenCount INTEGER,
    reasoningTokenCount INTEGER,
    generationDurationMs INTEGER,
    status TEXT NOT NULL,
    participant TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    thoughtTimeMs INTEGER,
    modelName TEXT,
    toolCallJson TEXT,
    attachmentMeta TEXT,
    runId TEXT NOT NULL,
    runSequence INTEGER NOT NULL,
    consumedAtPass INTEGER,
    sourceJson TEXT,
    PRIMARY KEY (id),
    FOREIGN KEY(conversationId) REFERENCES conversations(id) ON UPDATE NO ACTION ON DELETE CASCADE,
    FOREIGN KEY(runId) REFERENCES runs(id) ON UPDATE NO ACTION ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS index_messages_conversationId ON messages (conversationId);
CREATE INDEX IF NOT EXISTS index_messages_runId ON messages (runId);
CREATE INDEX IF NOT EXISTS index_messages_id_images_attachmentMeta
    ON messages (id, images, attachmentMeta);

CREATE TABLE IF NOT EXISTS new_chat_persist (
    id INTEGER NOT NULL,
    modelId TEXT,
    systemPromptId TEXT,
    conversationSettingsJson TEXT,
    draftText TEXT NOT NULL,
    draftAttachments TEXT,
    PRIMARY KEY (id)
);
