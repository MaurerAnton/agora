package com.newoether.agora.model

object OpenAiServiceTiers {
    const val AUTO = "auto"
    const val DEFAULT = "default"
    const val FLEX = "flex"
    const val SCALE = "scale"
    const val PRIORITY = "priority"
    const val FAST = "fast"
    const val ULTRAFAST = "ultrafast"

    val values = listOf(AUTO, DEFAULT, FLEX, SCALE, PRIORITY, FAST, ULTRAFAST)

    fun normalize(value: String?): String =
        value?.trim()?.lowercase()?.takeIf { it in values } ?: AUTO

    fun indexForTier(value: String?): Int = values.indexOf(normalize(value))

    fun tierForIndex(index: Int): String = values[index.coerceIn(values.indices)]

    fun requestValue(
        enabled: Boolean,
        value: String?,
        responsesApiEnabled: Boolean,
    ): String? = normalize(value).takeIf { enabled && responsesApiEnabled }
}
