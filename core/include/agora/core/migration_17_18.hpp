#pragma once

#include "agora/core/sqlite_platform.hpp"

namespace agora::core::migration_17_18 {

// Port of data/local/migration/Migration17To18.kt: repairs the v17 shape where Regenerate was
// persisted like Edit. The planner (module 41) decides the repair; this port emits the exact SQL
// that reparents Runs, deletes cloned inputs, renumbers sequences and migrates selections.

using sqlite_platform::Database;
using sqlite_platform::MigrationException;
using sqlite_platform::QueryResult;
using sqlite_platform::SqlValue;

/** MIGRATION_17_18.migrate. */
void migrate_17_18(Database& database);

}  // namespace agora::core::migration_17_18
