package com.newoether.agora.remote

import kotlinx.serialization.Serializable

/** Authenticated protocol-2 discovery; absent capabilities never grant a native operation. */
@Serializable
internal data class RemoteCapabilities(
    val create: Boolean = false, val history: Boolean = false, val live: Boolean = false,
    val idleSend: Boolean = false, val activeSend: String = "unsupported", val stop: Boolean = false,
    val settings: List<String> = emptyList(), val rename: Boolean = false, val archive: Boolean = false,
    val uploads: Boolean = false, val models: Boolean = false, val usage: Boolean = false,
    val lazyMessages: Boolean = false, val historyMode: String = "bounded",
) {
    fun intersect(other: RemoteCapabilities) = RemoteCapabilities(
        create = create && other.create, history = history && other.history, live = live && other.live,
        idleSend = idleSend && other.idleSend,
        activeSend = if (activeSend == "native-steer" && other.activeSend == "native-steer") "native-steer" else "unsupported",
        stop = stop && other.stop, settings = settings.intersect(other.settings.toSet()).toList(),
        rename = rename && other.rename, archive = archive && other.archive, uploads = uploads && other.uploads,
        models = models && other.models, usage = usage && other.usage, lazyMessages = lazyMessages && other.lazyMessages,
        historyMode = if (historyMode == "complete" && other.historyMode == "complete") "complete" else "bounded",
    )

    companion object {
        val legacy = RemoteCapabilities(true, true, true, true, "native-steer", true,
            listOf("model", "effort", "serviceTier"), true, true, true, true, true, true, "complete")
    }
}

@Serializable
internal data class RemoteAdapter(
    val id: String, val family: String, val origin: String, val state: String,
    val error: String? = null, val nativeScopeId: String = "",
    val capabilities: RemoteCapabilities = RemoteCapabilities(),
) {
    val legacy: Boolean get() = id == LEGACY_DESKTOP
    val status: RemoteDeviceStatus get() = remoteNativeStatus(state)
    companion object {
        const val LEGACY_DESKTOP = "legacyDesktop"
        fun legacy(status: RemoteDeviceStatus) = RemoteAdapter(LEGACY_DESKTOP, "codex", "desktop",
            when (status) {
                RemoteDeviceStatus.CONNECTED -> "connected"
                RemoteDeviceStatus.CONNECTING -> "connecting"
                RemoteDeviceStatus.ERROR -> "unavailable"
                RemoteDeviceStatus.IDLE -> "disconnected"
            }, capabilities = RemoteCapabilities.legacy)
    }
}

@Serializable
internal data class RemoteSessionAccess(
    val nativeScopeId: String, val canonicalSessionRef: String, val binding: String,
    val capabilities: RemoteCapabilities = RemoteCapabilities(),
)

/** A captured route is never inferred from a canonical conversation or the currently selected source. */
internal data class RemoteAccessPath(
    val adapterId: String, val binding: String? = null, val nativeScopeId: String = "",
) {
    init {
        require(adapterId.matches(Regex("[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}")))
        require(binding == null || binding.isNotBlank() && binding.length <= 8192 && binding.none { it.isISOControl() })
    }
    fun route(path: String): String = "v1/adapters/$adapterId/" + path.removePrefix("v1/")
}

internal fun remoteNativeStatus(state: String?): RemoteDeviceStatus = when (state) {
    "connected" -> RemoteDeviceStatus.CONNECTED
    "connecting" -> RemoteDeviceStatus.CONNECTING
    "unavailable" -> RemoteDeviceStatus.ERROR
    else -> RemoteDeviceStatus.IDLE
}

internal fun remoteIdentity(vararg parts: String): String = parts.joinToString("") { "${it.length}:$it" }
