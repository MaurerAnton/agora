// Stand-local SettingsRepository surface used by ConversationSettingsTransferCoordinator.
//
// applyConversationSettingsImportAndAwait runs the real ConversationSettingsState.applyImport
// (normalisation, replace/merge, pending-version bookkeeping) and persists the result through the
// stand SettingsManager exactly like the app repository's saveConversationSettingsMap call. The
// DataStore read/write itself is the substituted boundary. A scenario can make awaitInitialLoad
// or the import apply fail to pin DataImporter's two error branches.
package com.newoether.agora.data.repository

import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.SettingsManager
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

class SettingsRepository(
    private val settingsManager: SettingsManager,
    private val failInitialLoad: String? = null,
    private val failImportApply: String? = null,
) {
    private val conversationSettingsWriteMutex = Mutex()
    private val conversationSettingsState = ConversationSettingsState()

    val conversationSettings: Map<String, ConversationSettings>
        get() = conversationSettingsState.state.value

    val appliedImports = mutableListOf<Pair<Boolean, Map<String, ConversationSettings>>>()

    suspend fun awaitInitialLoad() {
        failInitialLoad?.let { throw IllegalStateException(it) }
    }

    suspend fun setConversationSettingsAndAwait(
        convId: String,
        settings: ConversationSettings?,
    ) {
        conversationSettingsState.set(convId, settings)
        val updated = conversationSettingsState.state.value
        settingsManager.saveConversationSettingsMap(updated)
        conversationSettingsState.acceptPersisted(updated)
    }

    suspend fun applyConversationSettingsImportAndAwait(
        imported: Map<String, ConversationSettings>,
        replace: Boolean,
    ) {
        conversationSettingsWriteMutex.withLock {
            failImportApply?.let { throw IllegalStateException(it) }
            val updated = conversationSettingsState.applyImport(imported, replace)
            settingsManager.saveConversationSettingsMap(updated)
            conversationSettingsState.acceptPersisted(updated)
            appliedImports += replace to updated
        }
    }
}
