// Stand-only leaf stub: the three flags plus the tool timeout that ShellToolDefinitions.build()
// and ShellDurableJobExecutor.maxWaitMs() read. Everything else of the real GenerationContext
// (Compose state, project, model settings) is irrelevant to the pinned logic.
package com.newoether.agora.viewmodel

import com.newoether.agora.data.ShellDeviceConfig

data class GenerationContext(
    val shellEnabled: Boolean = false,
    val shellDevices: List<ShellDeviceConfig> = emptyList(),
    val sandboxEnabled: Boolean = false,
    val toolTimeoutMs: Int = 60_000,
)
