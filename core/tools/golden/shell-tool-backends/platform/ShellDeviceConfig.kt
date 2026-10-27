// Stand-only leaf stub: the ShellDeviceConfig fields ShellToolBackends.kt reads — the Conch
// half (serverUrl/apiKey/conchPublicKey) and the SSH half (sshHost/sshPort/sshUser/sshPassword/
// sshHostKey) plus name/description/type. The real class lives in data/SettingsContracts.kt
// next to the settings schema this module never touches.
package com.newoether.agora.data

data class ShellDeviceConfig(
    val name: String,
    val description: String = "",
    val type: String = "conch",
    val serverUrl: String = "",
    val apiKey: String = "",
    val conchPublicKey: String = "",
    val sshHost: String = "",
    val sshPort: Int = 22,
    val sshUser: String = "root",
    val sshPassword: String = "",
    val sshHostKey: String = "",
)
