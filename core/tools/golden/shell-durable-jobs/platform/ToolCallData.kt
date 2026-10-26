package com.newoether.agora.model

/**
 * Stand-local leaf: the three fields terminalShellJobAcknowledgements reads — toolName selects
 * the acknowledgeable shell tools, result carries the durable-job envelope and arguments carry
 * the original server name. The real ToolCallData in ChatMessage.kt additionally carries
 * signature, images and protocol-side fields.
 */
data class ToolCallData(
    val toolName: String,
    val arguments: String,
    val result: String,
)
