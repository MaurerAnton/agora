-- Schema for the semantic index ledger stand.
--
-- The two ledger tables and the embeddings table reproduce the Room-generated DDL of
-- SemanticIndexLedgerEntity, SemanticIndexWorkEntity and EmbeddingEntity (column order, nullability,
-- primary keys, the unique (messageId, modelId) index and the ON DELETE CASCADE foreign key).
--
-- `messages` and `conversations` belong to other modules. The ledger only reads four message
-- columns (id, conversationId, text, participant) and one conversation column (taskId), so the
-- stand declares exactly that observable subset; it is the documented boundary of this module.
CREATE TABLE IF NOT EXISTS conversations (
    id TEXT NOT NULL,
    taskId TEXT,
    PRIMARY KEY (id)
);

CREATE TABLE IF NOT EXISTS messages (
    id TEXT NOT NULL,
    conversationId TEXT NOT NULL,
    text TEXT NOT NULL,
    participant TEXT NOT NULL,
    PRIMARY KEY (id)
);

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

CREATE TABLE IF NOT EXISTS semantic_index_ledger (
    modelId TEXT NOT NULL,
    state TEXT NOT NULL,
    sourceRevision INTEGER NOT NULL,
    completedRevision INTEGER NOT NULL,
    reconcileRevision INTEGER NOT NULL,
    updatedAt INTEGER NOT NULL,
    PRIMARY KEY (modelId)
);

CREATE TABLE IF NOT EXISTS semantic_index_work (
    modelId TEXT NOT NULL,
    messageId TEXT NOT NULL,
    sourceFingerprint TEXT,
    sourceRevision INTEGER NOT NULL,
    updatedAt INTEGER NOT NULL,
    PRIMARY KEY (modelId, messageId),
    FOREIGN KEY (modelId) REFERENCES semantic_index_ledger (modelId)
        ON UPDATE NO ACTION ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS index_semantic_index_work_modelId_sourceRevision_messageId
    ON semantic_index_work (modelId, sourceRevision, messageId);
