#pragma once

#include "agora/core/sqlite_platform.hpp"

namespace agora::core::migration_16_17 {

// Port of data/local/migration/Migration16To17.kt: the v16 message forest is read per
// conversation, the LegacyRunBackfillPlanner builds the Run plan, and the migration emits the
// exact DDL/DML that creates v17 Runs, assigns messages and migrates message selections.

using sqlite_platform::Database;
using sqlite_platform::MigrationException;
using sqlite_platform::QueryResult;
using sqlite_platform::SqlValue;

/**
 * MIGRATION_16_17.migrate: ALTER/CREATE the v17 schema, insert planned Runs, assign every legacy
 * message, migrate selections and verify that no message is left without a Run.
 */
void migrate_16_17(Database& database);

}  // namespace agora::core::migration_16_17
