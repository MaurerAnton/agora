package com.newoether.agora.model

/**
 * Allowed output set for the auto-regenerate guard (issue #116 B2): Latin (including its
 * extended blocks and ligatures), basic Cyrillic with its supplement, Arabic with its
 * presentation forms, the combining marks those scripts use, and Unicode's Common blocks —
 * digits, punctuation, arrows, math, technical and pictographic symbols, emoji. Everything
 * else means the answer drifted into a language this UI does not speak: Han, kana, hangul,
 * CJK punctuation, Devanagari, archaic "vyaz" Cyrillic outside the basic block, Greek, and
 * anything a future script adds — one whitelist rule instead of a hardcoded list of scripts.
 *
 * Matching runs over whole code points so supplementary-plane emoji stay quiet while a
 * supplementary ideograph still triggers (a UTF-16 char loop sees only the surrogate halves).
 */
private val ALLOWED_CODE_POINT_RANGES = listOf(
    0x0020..0x007E,     // ASCII letters, digits, punctuation and symbols
    0x00A0..0x02FF,     // Latin-1 supplement through Latin Ext-B, IPA, modifier letters
    0x0300..0x036F,     // combining diacritics shared by Latin and Cyrillic
    0x0400..0x04FF,     // basic Cyrillic (archaic vyaz lives in the Extended blocks)
    0x0500..0x052F,     // Cyrillic Supplement
    0x0600..0x06FF,     // Arabic
    0x0750..0x077F,     // Arabic Supplement
    0x08A0..0x08FF,     // Arabic Extended-A
    0x1E00..0x1EFF,     // Latin Extended Additional (Vietnamese)
    0x2000..0x206F,     // General Punctuation (em dash, quotes, bullets, ZWSP)
    0x2070..0x209F,     // superscripts and subscripts
    0x20A0..0x20CF,     // currency symbols
    0x2100..0x218F,     // letterlike symbols and number forms
    0x2190..0x21FF,     // arrows
    0x2200..0x22FF,     // mathematical operators
    0x2300..0x23FF,     // miscellaneous technical (✓, ⏳, keyboard glyphs)
    0x2400..0x24FF,     // control pictures and enclosed alphanumerics
    0x2500..0x25FF,     // box drawing, block elements, geometric shapes
    0x2600..0x27BF,     // miscellaneous symbols and dingbats
    0x2C60..0x2C7F,     // Latin Extended-C
    0xA720..0xA7FF,     // Latin Extended-D
    0xFB00..0xFB0F,     // Latin ligatures (ﬀ ﬀ)
    0xFB50..0xFDFF,     // Arabic Presentation Forms-A
    0xFE70..0xFEFF,     // Arabic Presentation Forms-B
    0xFE00..0xFE0F,     // variation selectors (emoji sequences)
    0x1D400..0x1D7FF,   // Mathematical Alphanumeric Symbols
    0x1F000..0x1FAFF,   // emoji and pictographs
)

/** True when [this] contains at least one code point outside the allowed output set. */
fun String.containsCjk(): Boolean {
    var index = 0
    while (index < length) {
        val codePoint = Character.codePointAt(this, index)
        index += Character.charCount(codePoint)
        if (ALLOWED_CODE_POINT_RANGES.none { codePoint in it }) return true
    }
    return false
}

/**
 * API-only user instruction appended when retrying a generation aborted for CJK output.
 * Never persisted: it only steers the retry request.
 */
const val CJK_RETRY_GUARD_PROMPT =
    "Important: do not use any Chinese (CJK) characters anywhere in your answer."
