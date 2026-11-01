package com.newoether.agora.data.repository

import com.newoether.agora.data.ShellDeviceConfig
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

/** Reports the outcome of the repository-owned write, whose Job outlives the calling editor. */
internal fun SettingsRepository.persistShellDevice(
    device: ShellDeviceConfig,
    creating: Boolean,
    onResult: (Boolean) -> Unit,
    onError: (Exception) -> Unit,
) = scope.launch {
    val saved = try {
        if (creating) settingsManager.shellDeviceStore.add(device)
        else settingsManager.shellDeviceStore.update(device)
    } catch (cancelled: CancellationException) {
        throw cancelled
    } catch (error: Exception) {
        onError(error)
        return@launch
    }
    // Consumer failures must not be reclassified as storage errors or delivered twice.
    onResult(saved)
}
