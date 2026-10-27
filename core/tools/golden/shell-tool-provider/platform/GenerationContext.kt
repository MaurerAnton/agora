// Stand-only leaf stub: the GenerationContext fields tool/ShellToolProvider.kt reads — the shell
// flags and device list, the sandbox flags, the conversation id handed to the confirmation gate
// and the tool timeout budget consumed by ShellDurableJobExecutor.maxWaitMs(). Everything else of
// the real GenerationContext in viewmodel/GenerationContracts.kt (Compose state, model settings,
// provider snapshots) is irrelevant to the pinned dispatch logic.
package com.newoether.agora.viewmodel

import com.newoether.agora.data.ShellDeviceConfig

data class GenerationContext(
    val conversationId: String? = null,
    val shellEnabled: Boolean = false,
    val shellDevices: List<ShellDeviceConfig> = emptyList(),
    val sandboxEnabled: Boolean = false,
    val sandboxSharedStorageEnabled: Boolean = false,
    val toolTimeoutMs: Long = 60_000L,
)
