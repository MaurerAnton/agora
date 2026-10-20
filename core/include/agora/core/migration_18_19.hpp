#pragma once

#include "agora/core/sqlite_platform.hpp"

namespace agora::core::migration_18_19 {

// Port of data/local/migration/Migration18To19.kt: forward-repairs databases that opened the
// rejected first v18 build by re-running the v18 regeneration edge repair and applying its
// message parent updates.

using sqlite_platform::Database;
using sqlite_platform::MigrationException;
using sqlite_platform::QueryResult;
using sqlite_platform::SqlValue;

/** MIGRATION_18_19.migrate. */
void migrate_18_19(Database& database);

}  // namespace agora::core::migration_18_19
