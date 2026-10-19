#pragma once

#include <string_view>

namespace agora::core::cjk_text {

/**
 * Port of model/CjkText.kt: the Han-ideograph probe behind the auto-regenerate guard, and the
 * API-only instruction appended when retrying a generation that was aborted for CJK output.
 *
 * The probe is a UTF-16 `Char` range test in Kotlin, which matters: only three BMP blocks count,
 * Hiragana/Katakana/Hangul and CJK punctuation are deliberately excluded, and a supplementary-plane
 * ideograph arrives as a surrogate pair whose halves miss every range. Decoding UTF-8 and testing
 * the full code point gives the same answer for every valid string, and the golden traces pin the
 * supplementary cases explicitly.
 */

/** Never persisted: it only steers the retry request. */
inline constexpr std::string_view kRetryGuardPrompt =
    "Important: do not use any Chinese (CJK) characters anywhere in your answer.";

/** True when the text contains at least one Han ideograph from the three pinned BMP blocks. */
bool contains_cjk(std::string_view text);

}  // namespace agora::core::cjk_text
