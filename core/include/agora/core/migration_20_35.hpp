#pragma once

#include "agora/core/sqlite_platform.hpp"

namespace agora::core::migration_20_35 {

// Ports of data/local/migration/Migration20To21.kt .. Migration34To35.kt: the SQL-only schema
// upgrades from v20 through v35. Each function mirrors one Room Migration and emits the same
// ordered execSQL calls.

using sqlite_platform::Database;
using sqlite_platform::MigrationException;
using sqlite_platform::QueryResult;
using sqlite_platform::SqlValue;

void migrate_20_21(Database& database);
void migrate_21_22(Database& database);
void migrate_22_23(Database& database);
void migrate_23_24(Database& database);
void migrate_24_25(Database& database);
void migrate_25_26(Database& database);
void migrate_26_27(Database& database);
void migrate_27_28(Database& database);
void migrate_28_29(Database& database);
void migrate_29_30(Database& database);
void migrate_30_31(Database& database);
void migrate_31_32(Database& database);
void migrate_32_33(Database& database);
void migrate_33_34(Database& database);
void migrate_34_35(Database& database);

/** Runs the migration whose source schema version is `from_version` (20..34). */
bool migrate(int from_version, Database& database);

}  // namespace agora::core::migration_20_35
