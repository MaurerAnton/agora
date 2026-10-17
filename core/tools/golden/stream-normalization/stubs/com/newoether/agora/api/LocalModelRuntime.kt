package com.newoether.agora.api

/**
 * Host-stand replacement for the embedded llama runtime: the stand never has a resident model,
 * so exactTokenCountOrNull answers "no answer" exactly like the app does before the first
 * generation, and LocalModelTextCounter falls back to the offline heuristic.
 */
internal object LocalModelRuntime {

    fun exactTokenCountOrNull(text: String): Int? = null
}
