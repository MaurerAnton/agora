// Platform substitute for model/ChatMessage.kt's ToolCallDisplayModes: the settings schema
// validates the stored value against these constants; the rest of ChatMessage.kt stays out.
package com.newoether.agora.model

object ToolCallDisplayModes {
    const val TIMELINE = "timeline"
    const val GROUPED_TIMELINE = "grouped_timeline"
    const val DEFAULT = GROUPED_TIMELINE

    fun normalize(value: String?): String = when (value) {
        "compact" -> GROUPED_TIMELINE
        GROUPED_TIMELINE -> GROUPED_TIMELINE
        TIMELINE -> TIMELINE
        else -> DEFAULT
    }
}
