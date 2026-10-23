-- Schema for the context-compact stand.
--
-- `messages` reproduces the Room-generated DDL for MessageEntity: every column the entity declares
-- (the `images` list is stored by MessageConverters as a JSON array), both foreign keys with
-- ON DELETE CASCADE, and the three indices. `runs` is the Migration16To17 table RunEntity declares
-- (both foreign keys, the unique (conversationId, activeSlot) index where SQLite permits multiple
-- nulls). `embeddings` is the orphan surface deleteEmbeddingsByMessageIds touches. The
-- `conversations` table is the observable subset the compact transactions read and write
-- (documented in platform/StandChat.kt) plus lastUpdated/dataChangedAt, which the update queries
-- set.
CREATE TABLE IF NOT EXISTS conversations (
    id TEXT NOT NULL,
    selectedBranchesJson TEXT,
    selectedRunBranchesJson TEXT,
    lastUpdated INTEGER NOT NULL DEFAULT 0,
    dataChangedAt INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (id)
);

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
