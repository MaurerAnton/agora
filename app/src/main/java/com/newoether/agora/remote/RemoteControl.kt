package com.newoether.agora.remote

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch

/** Captured native controls keep their canonical barrier until the original source settles. */
internal class RemoteControl(
    private val scope: CoroutineScope,
    private val mutableState: MutableStateFlow<RemoteState>,
    private val clientFor: (RemoteState) -> FiloClient?,
    private val sameSource: (RemoteState, FiloClient) -> Boolean,
    private val settingsConflict: (RemoteState) -> Boolean,
    private val selectionEpoch: () -> Long,
    private val report: (Exception) -> RemoteFailure?,
) {
    private data class Stop(val path: String?, val turn: String, val selection: Long,
        val receipt: Boolean = false, val ended: Boolean = false)
    private val stops = mutableMapOf<String, Stop>()

    fun settle(runtime: RemoteRuntime? = mutableState.value.runtime) {
        val snapshot = mutableState.value
        val owner = snapshot.owner ?: return
        val stop = stops[owner]?.takeIf { it.path == snapshot.confirmationPath &&
            (!snapshot.selectedAdapter.legacy || it.selection == selectionEpoch()) } ?: return
        val ended = stop.ended || runtime?.let {
            it.status in setOf("idle", "ready") || it.completedTurnId == stop.turn ||
                it.isRunning && it.activeTurnId != null && it.activeTurnId != stop.turn
        } == true
        if (ended && stop.receipt) {
            stops.remove(owner)
            mutableState.value = snapshot.copy(pendingControls = snapshot.pendingControls - owner,
                stoppingOwner = null, stoppingTurnId = null, stoppingTurnEnded = false)
        } else {
            stops[owner] = stop.copy(ended = ended)
            mutableState.value = snapshot.copy(stoppingOwner = owner, stoppingAccessKey = stop.path,
                stoppingTurnId = stop.turn, stoppingTurnEnded = ended)
        }
    }

    fun run(
        snapshot: RemoteState = mutableState.value, stoppingTurnId: String? = null,
        clientOverride: FiloClient? = null, operation: suspend (FiloClient) -> Unit,
    ) {
        if (snapshot.controlling || snapshot.controlPending || snapshot.isStopping || settingsConflict(snapshot)) return
        val client = clientOverride ?: clientFor(snapshot) ?: return
        val selected = selectionEpoch()
        val owner = snapshot.owner
        if (owner != null && stoppingTurnId != null) stops[owner] = Stop(snapshot.confirmationPath, stoppingTurnId, selected)
        mutableState.value = mutableState.value.copy(controlling = true,
            pendingControls = if (owner == null || snapshot.selectedAdapter.legacy) mutableState.value.pendingControls else
                mutableState.value.pendingControls + (owner to snapshot.confirmationPath),
            stoppingOwner = if (stoppingTurnId != null) owner else mutableState.value.stoppingOwner,
            stoppingAccessKey = if (stoppingTurnId != null) snapshot.confirmationPath else mutableState.value.stoppingAccessKey,
            stoppingTurnId = stoppingTurnId ?: mutableState.value.stoppingTurnId, stoppingTurnEnded = false)
        scope.launch {
            var succeeded = false
            try {
                if (!sameSource(snapshot, client)) return@launch
                operation(client)
                succeeded = sameSource(snapshot, client)
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) {
                if (selected == selectionEpoch() && sameSource(snapshot, client)) {
                    val failure = report(error)
                    mutableState.value = mutableState.value.copy(failure = failure)
                }
            } finally {
                if (owner != null && (stoppingTurnId == null || stops[owner]?.selection == selected)) {
                    if (stoppingTurnId != null && succeeded) {
                        stops[owner]?.let { stops[owner] = it.copy(receipt = true) }
                    } else {
                        stops.remove(owner)
                        mutableState.value = mutableState.value.copy(pendingControls = mutableState.value.pendingControls - owner)
                    }
                }
                if (selected == selectionEpoch()) {
                    mutableState.value = mutableState.value.copy(controlling = false,
                        settingsRevision = mutableState.value.settingsRevision + 1)
                    if (stoppingTurnId != null && !succeeded) mutableState.value = mutableState.value.copy(
                        stoppingOwner = null, stoppingTurnId = null, stoppingTurnEnded = false)
                    settle()
                }
            }
        }
    }
}
