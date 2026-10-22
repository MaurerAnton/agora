// Stand-local conversation surface for compiling data/local/RunEntity.kt and the maintenance
// repair verbatim.
//
// RunEntity names ChatEntity only as the parent of its conversationId foreign key, and
// MaintenanceDebtDao reads exactly one conversation property (selectedRunBranchesJson) while the
// compare-and-set query touches dataChangedAt in SQL. The stand declares that observable subset;
// the full conversation table belongs to other modules.
package com.newoether.agora.data.local

import androidx.room.Entity
import androidx.room.PrimaryKey

@Entity(tableName = "conversations")
data class ChatEntity(
    @PrimaryKey val id: String,
    val selectedRunBranchesJson: String? = null,
)
