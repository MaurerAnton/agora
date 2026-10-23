// Stand-local conversation surface for compiling data/local/ChatContextCompactDao.kt verbatim.
//
// The compact graph transactions read exactly two conversation properties (selectedBranchesJson
// and selectedRunBranchesJson); the update queries additionally write lastUpdated and
// dataChangedAt in SQL. The stand declares that observable subset; the full conversation table
// belongs to other modules.
package com.newoether.agora.data.local

import androidx.room.Entity
import androidx.room.PrimaryKey

@Entity(tableName = "conversations")
data class ChatEntity(
    @PrimaryKey val id: String,
    val selectedBranchesJson: String? = null,
    val selectedRunBranchesJson: String? = null,
)
