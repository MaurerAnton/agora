package com.mikepenz.markdown.model

import kotlinx.serialization.Serializable

/**
 * Stand-in for the markdown library's prepared-state marker.
 *
 * ChatMessage.preparedMarkdown is a disposable UI cache the preparation pipeline never reads or
 * writes; the stub only has to satisfy the model's serialization. Scenarios never set it.
 */
object State {

    @Serializable
    class Success(val marker: String = "")
}
