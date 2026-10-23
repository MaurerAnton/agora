-- Schema for the chat-search-dao stand (data/local/ChatSearchDao.kt).
--
-- The four tables reproduce the Room-generated DDL for the entities the DAO touches:
-- ChatEntity (`conversations`), RunEntity (`runs`), MessageEntity (`messages`) and
-- EmbeddingEntity (`embeddings`). Every declared column is present with its nullability, both
-- foreign keys keep ON DELETE CASCADE and the standalone indices are created, so the declared
-- @Query SQL runs against the same physical shape as the app database. `runs` is never read by
-- the DAO; it exists because `messages.runId` references it and the stand keeps foreign keys ON.
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

CREATE TABLE IF NOT EXISTS embeddings (
    id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
    messageId TEXT NOT NULL,
    modelId TEXT NOT NULL,
    embedding BLOB NOT NULL,
    chunkText TEXT NOT NULL,
    dimension INTEGER NOT NULL,
    sourceFingerprint TEXT
);

CREATE UNIQUE INDEX IF NOT EXISTS index_embeddings_messageId_modelId
    ON embeddings (messageId, modelId);

CREATE INDEX IF NOT EXISTS index_embeddings_modelId ON embeddings (modelId);
