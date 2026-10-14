package com.newoether.agora.model

/**
 * CJK ranges treated as "Chinese characters" for the auto-regenerate guard:
 * Extension A, the main unified block, and the compatibility block.
 * Hiragana/katakana/hangul are deliberately excluded: only Han ideographs trigger.
 */
private val CJK_RANGES = listOf(
    '\u3400'..'\u4DBF',
    '\u4E00'..'\u9FFF',
    '\uF900'..'\uFAFF',
)

/** True when [this] contains at least one Han ideograph. Supplementary planes are ignored. */
fun String.containsCjk(): Boolean = any { char -> CJK_RANGES.any { char in it } }

/**
 * API-only user instruction appended when retrying a generation aborted for CJK output.
 * Never persisted: it only steers the retry request.
 */
const val CJK_RETRY_GUARD_PROMPT =
    "Important: do not use any Chinese (CJK) characters anywhere in your answer."
