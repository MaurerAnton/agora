package com.newoether.agora.data

/**
 * Stand-local leaf: name and type, the only ShellDeviceConfig fields the durable-job executor
 * reads (name-based lookup with the "ssh" exclusion). The real class in SettingsContracts.kt
 * additionally carries the URL/key and SSH fields.
 */
data class ShellDeviceConfig(
    val name: String,
    val type: String = "conch",
)
