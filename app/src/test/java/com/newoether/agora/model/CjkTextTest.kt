package com.newoether.agora.model

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CjkTextTest {
    @Test
    fun plainLatinAndCyrillicHaveNoCjk() {
        assertFalse("Hello world!".containsCjk())
        assertFalse("Привет, как дела?".containsCjk())
        assertFalse("123 😀 مرحبا".containsCjk())
    }

    @Test
    fun hanIdeographsDetected() {
        assertTrue("中文".containsCjk())
        assertTrue("The answer is 42 你好".containsCjk())
        assertTrue("ぃ㐀".containsCjk())
    }

    @Test
    fun japaneseKanaAndKoreanHangulDoNotTrigger() {
        assertFalse("こんにちは".containsCjk())
        assertFalse("カタカナ".containsCjk())
        assertFalse("한글 테스트".containsCjk())
    }
}
