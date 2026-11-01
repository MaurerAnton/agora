package com.newoether.agora.remote

/** Unknown reads retain confirmed presentation; an exact terminal identity can settle it. */
internal fun RemoteState.mergeListedSessionStatuses(
    address: String, page: RemoteSessionPage,
): Map<String, RemoteSessionStatus> {
    val exact = page.statuses.associateBy { it.id }
    val listed = page.sessions.mapNotNull { session ->
        val key = sessionKey(session.id)
        val old = sessionStatuses[key]
        val item = exact[session.id]
            ?: session.status?.let { old?.copy(status = it) ?: RemoteSessionStatus(session.id, it) }
            ?: return@mapNotNull null
        val confirmed = if (item.status == "running") item.copy(status = "active") else item
        key to reconcileRemoteSessionStatus(old, confirmed)
    }.toMap()
    return sessionStatuses + listed
}

internal fun reconcileRemoteSessionStatus(
    old: RemoteSessionStatus?, item: RemoteSessionStatus,
): RemoteSessionStatus {
    val confirmed = item.status in setOf("idle", "active", "ready")
    val resolved = if (!confirmed && old != null) {
        if (item.completedTurnId != null && item.completedTurnId == old.activeTurnId) {
            item.copy(status = "idle", activeTurnId = null)
        } else old
    } else item
    return resolved.copy(hasUnreadTurn = resolved.hasUnreadTurn ||
        item.completedTurnId != null && (old?.activeTurnId == item.completedTurnId &&
            old.completedTurnId != item.completedTurnId ||
            old?.hasUnreadTurn == true && old.completedTurnId == item.completedTurnId))
}
