#pragma once

#include <string_view>

namespace agora::core::cjk_text {

/**
 * Port of model/CjkText.kt: the allowed-output-set probe behind the auto-regenerate guard,
 * and the API-only instruction appended when retrying a generation that was aborted for CJK
 * output.
 *
 * The probe whitelists Latin, basic Cyrillic, Arabic, their combining marks and Unicode's
 * Common blocks (punctuation, digits, arrows, math, emoji) and triggers on anything else —
 * Han, kana, hangul, CJK punctuation, Devanagari, archaic vyaz Cyrillic outside the basic
 * block, Greek, and every future script (issue #116 B2). Kotlin walks UTF-16 by code point
 * and the port decodes UTF-8; both must agree for every valid string, and the golden traces
 * pin the boundary, supplementary and emoji cases explicitly.
 */

/** Never persisted: it only steers the retry request. */
inline constexpr std::string_view kRetryGuardPrompt =
    "Important: do not use any Chinese (CJK) characters anywhere in your answer.";

/** True when the text contains at least one code point outside the allowed output set. */
bool contains_cjk(std::string_view text);

}  // namespace agora::core::cjk_text
