// Stand-local conversation surface for compiling the recovery body verbatim.
//
// The recovery body reads exactly one conversation property (selectedRunBranchesJson) while the
// compare-and-set and touch queries write dataChangedAt in SQL. The stand declares that observable
// subset; the full conversation table belongs to other modules.
package com.newoether.agora.data.local

import androidx.room.Entity
import androidx.room.PrimaryKey

@Entity(tableName = "conversations")
data class ChatEntity(
    @PrimaryKey val id: String,
    val selectedRunBranchesJson: String? = null,
)
