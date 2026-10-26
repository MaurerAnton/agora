// Stand-local ConversationRepository surface used by ConversationSettingsTransferCoordinator.
//
// Every method delegates to the real ChatDao query with the same mapping the app repository
// performs (delete > 0 -> Boolean). The Room/DataStore persistence behind ChatDao is the
// substituted boundary; no transfer policy lives here.
package com.newoether.agora.data.repository

import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.local.ConversationSettingsImportTransferEntity
import com.newoether.agora.data.local.ConversationSettingsTransferEntity

class ConversationRepository(private val chatDao: ChatDao) {
    suspend fun getConversationSettingsTransfer(
        conversationId: String,
    ): ConversationSettingsTransferEntity? =
        chatDao.getConversationSettingsTransfer(conversationId)

    suspend fun getPendingConversationSettingsTransfers(): List<ConversationSettingsTransferEntity> =
        chatDao.getPendingConversationSettingsTransfers()

    suspend fun deleteConversationSettingsTransfer(conversationId: String): Boolean =
        chatDao.deleteConversationSettingsTransfer(conversationId) > 0

    suspend fun getConversationSettingsImportTransfer(): ConversationSettingsImportTransferEntity? =
        chatDao.getConversationSettingsImportTransfer()

    suspend fun deleteConversationSettingsImportTransfer(transferId: String): Boolean =
        chatDao.deleteConversationSettingsImportTransfer(transferId) > 0
}
