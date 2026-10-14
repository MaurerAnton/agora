package com.newoether.agora.api.openai

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CustomOpenAiProviderTest {
    @Test
    fun qwen38ThinkingToggleAndEffortUseSupportedValues() {
        assertEquals("none", qwen38ReasoningEffort("qwen3.8-27b", false, "max"))
        assertEquals("low", qwen38ReasoningEffort("qwen3.8-27b", true, "minimal"))
        assertEquals("low", qwen38ReasoningEffort("qwen3.8-27b", true, "low"))
        assertEquals("medium", qwen38ReasoningEffort("qwen3.8-27b", true, "medium"))
        assertEquals("xhigh", qwen38ReasoningEffort("qwen3.8-27b", true, "high"))
        assertEquals("xhigh", qwen38ReasoningEffort("qwen3.8-27b", true, "xhigh"))
        assertEquals("xhigh", qwen38ReasoningEffort("qwen3.8-27b", true, "max"))
    }

    @Test
    fun qwen38DetectionHandlesRawOwnerPathAndCase() {
        assertEquals(
            "medium",
            qwen38ReasoningEffort("Qwen/QWEN3.8-27B", true, "medium"),
        )
        assertNull(qwen38ReasoningEffort("provider-id:QWEN3.8-27B", true, "medium"))
    }

    @Test
    fun unrelatedCustomModelsRemainUntouched() {
        assertNull(qwen38ReasoningEffort("deepseek-v4", false, "medium"))
        assertNull(qwen38ReasoningEffort("qwen3.6-40b", true, "high"))
    }
}
