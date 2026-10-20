#pragma once

#include "agora/core/sqlite_platform.hpp"

namespace agora::core::migration_19_20 {

// Port of data/local/migration/Migration19To20.kt: repairs message rows poisoned before the
// persistence guard existed. Segment JSON is cleared (it cannot be byte-truncated without becoming
// invalid JSON); answer/reasoning text is clipped with the same truncation marker.

using sqlite_platform::Database;
using sqlite_platform::MigrationException;
using sqlite_platform::QueryResult;
using sqlite_platform::SqlValue;

/** MIGRATION_19_20.migrate. */
void migrate_19_20(Database& database);

}  // namespace agora::core::migration_19_20
