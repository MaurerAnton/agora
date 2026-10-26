package com.newoether.agora.viewmodel

import com.newoether.agora.data.ShellDeviceConfig

/**
 * Stand-local leaf: only the two fields ShellDurableJobExecutor.kt reads — shellDevices for
 * Conch device resolution and toolTimeoutMs for maxWaitMs. The real GenerationContext in
 * GenerationContracts.kt carries the whole generation configuration.
 */
data class GenerationContext(
    val shellDevices: List<ShellDeviceConfig> = emptyList(),
    val toolTimeoutMs: Long = 60_000L,
)
