package com.newoether.agora.data

/** Portable presentation preference; delegation and child ownership are independent. */
object SubagentCapsuleDisplayModes {
    const val INDIVIDUAL = "individual"
    const val GROUPED = "grouped"
    const val DEFAULT = GROUPED

    fun normalize(value: String?): String = when (value) {
        INDIVIDUAL, GROUPED -> value
        else -> DEFAULT
    }
}
