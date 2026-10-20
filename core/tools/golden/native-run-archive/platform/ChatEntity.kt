// Minimal stand-local conversation entity.
//
// data/local/RunEntity.kt names ChatEntity only as the parent of its conversationId foreign key.
// The archive policy never reads a conversation row, so the stand supplies just the table shape
// the Room annotation needs.
package com.newoether.agora.data.local

import androidx.room.Entity
import androidx.room.PrimaryKey

@Entity(tableName = "chats")
data class ChatEntity(
    @PrimaryKey val id: String,
)
