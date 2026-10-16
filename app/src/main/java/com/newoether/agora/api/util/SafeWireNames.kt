package com.newoether.agora.api.util

/**
 * Wire-format limits for tool call identities. A tool name or id outside these grammars cannot be
 * paired with a result reliably, so the consumer answers such a call with a malformed stand-in
 * instead of executing it (see [malformedToolCallRequest]).
 */
internal val safeWireToolName = Regex("[A-Za-z0-9_-]{1,64}")
internal val safeWireToolCallId = Regex("[A-Za-z0-9_-]{1,128}")
