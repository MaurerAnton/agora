-- Schema for the maintenance debt stand.
--
-- `maintenance_debt` reproduces Migration27To28's generated DDL (column order, composite primary
-- key, claim index); `runs` reproduces Migration16To17's table exactly as RunEntity declares it
-- (both foreign keys, the unique (conversationId, activeSlot) index where SQLite permits multiple
-- nulls); `embeddings`/`messages` carry the orphan query surface. The `conversations` table is
-- the observable subset the repair reads (documented in platform/StandChat.kt) plus dataChangedAt,
-- which the compare-and-set query writes.
CREATE TABLE IF NOT EXISTS conversations (
    id TEXT NOT NULL,
    selectedRunBranchesJson TEXT,
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

CREATE TABLE IF NOT EXISTS maintenance_debt (
    kind TEXT NOT NULL,
    identity TEXT NOT NULL,
    state TEXT NOT NULL,
    revision INTEGER NOT NULL,
    updatedAt INTEGER NOT NULL,
    claimId TEXT,
    claimedAt INTEGER,
    PRIMARY KEY(kind, identity)
);

CREATE INDEX IF NOT EXISTS index_maintenance_debt_state_updatedAt_kind_identity
    ON maintenance_debt (state, updatedAt, kind, identity);
