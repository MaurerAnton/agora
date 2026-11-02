package com.newoether.agora.model

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CjkTextTest {
    @Test
    fun plainLatinCyrillicArabicAndPunctuationHaveNoCjk() {
        assertFalse("Hello world!".containsCjk())
        assertFalse("Привет, как дела?".containsCjk())
        assertFalse("123 😀 مرحبا".containsCjk())
        assertFalse("Em dash — arrow → ≤ ≥ … ①② ✔ ⁠ZWSP​".containsCjk())
        assertFalse("Ligature ﬀ and é".containsCjk())
    }

    @Test
    fun hanIdeographsDetected() {
        assertTrue("中文".containsCjk())
        assertTrue("The answer is 42 你好".containsCjk())
        assertTrue("ぃ㐀".containsCjk())
    }

    @Test
    fun japaneseKanaKoreanHangulAndDevanagariTrigger() {
        assertTrue("こんにちは".containsCjk())
        assertTrue("カタカナ".containsCjk())
        assertTrue("한글 테스트".containsCjk())
        assertTrue("हिन्दी".containsCjk())
    }

    @Test
    fun archaicVyazAndGreekOutsideTheAllowedSetTrigger() {
        // Stylized Old Cyrillic "vyaz" lives in the Extended blocks, outside basic Cyrillic.
        assertTrue("Ѻꙑ".containsCjk())
        assertTrue("Ἑλληνικά".containsCjk())
        assertTrue("日本語の句読点。".containsCjk())
        assertTrue("ＡＢＣ".containsCjk())
        assertTrue("Supplementary ideograph \uD840\uDC00".containsCjk())
    }
}
