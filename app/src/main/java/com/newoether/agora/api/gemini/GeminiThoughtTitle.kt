package com.newoether.agora.api.gemini

// Gemini thought summaries carry their headline as **bold** or a markdown heading.
// Hoisted to file level: extraction runs on every streamed thought chunk, and Regex
// construction (Pattern.compile) is far too expensive to repeat per chunk.
private val THOUGHT_TITLE_BOLD = Regex("\\*\\*(.*?)\\*\\*")
private val THOUGHT_TITLE_HEADING = Regex("(?m)^#+\\s*(.*)$")

internal fun extractThoughtTitle(content: String): String? =
    THOUGHT_TITLE_BOLD.find(content)?.groupValues?.get(1)
        ?: THOUGHT_TITLE_HEADING.find(content)?.groupValues?.get(1)
