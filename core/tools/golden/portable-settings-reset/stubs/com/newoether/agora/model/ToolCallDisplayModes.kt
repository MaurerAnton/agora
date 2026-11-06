// Platform substitute for model/ChatMessage.kt's ToolCallDisplayModes: the schema's migration
// reads the retired display value, but the real object lives inside a model file whose closure
// (attachments, message sources, participant enums) is not part of this stand. The constants and
// normalize() are copied verbatim from the app object.
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
