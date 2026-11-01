package com.newoether.agora.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import com.newoether.agora.util.SecretCrypto
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/** Device names are the case-insensitive addresses exposed to shell tools. */
internal fun isShellDeviceNameAvailable(
    device: ShellDeviceConfig,
    devices: List<ShellDeviceConfig>,
): Boolean {
    val name = device.name.trim()
    return name.isNotEmpty() && !name.equals("Local Sandbox", ignoreCase = true) &&
        devices.none { it.id != device.id && it.name.trim().equals(name, ignoreCase = true) }
}

/** Serializes interactive device mutations against the current persisted list. */
internal class SettingsShellDeviceStore(
    private val store: DataStore<Preferences>,
    private val json: Json,
) {
    suspend fun add(device: ShellDeviceConfig): Boolean = save(device, creating = true)

    suspend fun update(device: ShellDeviceConfig): Boolean = save(device, creating = false)

    private suspend fun save(device: ShellDeviceConfig, creating: Boolean): Boolean {
        var saved = false
        store.edit { preferences ->
            val devices = decodeEncryptedShellDevices(preferences, json)
            val exists = devices.any { it.id == device.id }
            if (exists == creating || !isShellDeviceNameAvailable(device, devices)) return@edit
            val normalized = device.copy(name = device.name.trim())
            val updated = if (creating) devices + normalized else devices.map {
                if (it.id == device.id) normalized else it
            }
            val encrypted = try {
                encryptShellDevices(updated, json)
            } catch (_: IllegalStateException) {
                return@edit
            }
            preferences[SHELL_DEVICES_JSON] = encrypted
            saved = true
        }
        return saved
    }

    suspend fun remove(deviceId: String) {
        store.edit { preferences ->
            val devices = decodeEncryptedShellDevices(preferences, json)
            preferences[SHELL_DEVICES_JSON] = encryptShellDevices(
                devices.filterNot { it.id == deviceId }, json,
            )
        }
    }
}
