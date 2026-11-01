package com.newoether.agora.viewmodel

import com.newoether.agora.tool.SubagentToolProvider
import com.newoether.agora.util.Constants
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runTest
import org.junit.Assert.*
import org.junit.Test

class GenerationRequestBuilderSubagentModelsTest {
    @Test fun configuredCandidatesAndRecommendationAreFrozenSeparatelyFromAllEnabledModels() = runTest {
        val fixture = RequestBuilderFixture(Constants.PROVIDER_OPENAI, false)
        val enabled = MutableStateFlow(setOf("Z:last", fixture.modelId, "A:first"))
        val configured = MutableStateFlow<Set<String>?>(setOf("Z:last", "A:first", "Disabled:model"))
        val recommendation = MutableStateFlow<String?>("Z:last")
        every { fixture.settings.enabledModels } returns enabled
        every { fixture.settings.subagentEnabledModels } returns configured
        every { fixture.settings.subagentRecommendedModel } returns recommendation
        val admitted = fixture.builder.captureAdmissionSnapshot("conversation", "run", fixture.modelId)
        val projected = fixture.builder.captureContextProjectionSnapshot("conversation", fixture.modelId)
        for (context in listOf(admitted.context, projected.context)) {
            assertEquals(listOf("A:first", fixture.modelId, "Z:last"), context.enabledModelIds)
            assertEquals(listOf("A:first", "Z:last"), context.subagentModelIds)
            assertEquals("Z:last", context.subagentRecommendedModel)
            val description = SubagentToolProvider(mockk()).definitions(context).first().function.description
            assertFalse(description.contains(fixture.modelId))
            assertFalse(description.contains("Disabled:model"))
            assertTrue(description.contains("Recommended model ID"))
        }
        configured.value = emptySet()
        recommendation.value = null
        enabled.value = setOf(fixture.modelId)
        assertEquals(listOf("A:first", "Z:last"), admitted.context.subagentModelIds)
        assertEquals("Z:last", projected.context.subagentRecommendedModel)
        val next = fixture.builder.captureAdmissionSnapshot("conversation", "next-run", fixture.modelId)
        assertEquals(emptyList<String>(), next.context.subagentModelIds)
        assertNull(next.context.subagentRecommendedModel)
        assertEquals(listOf(fixture.modelId), next.context.enabledModelIds)
    }

    @Test fun unsetAndEmptyNeverInheritEnabledModelsAndInvalidateRecommendation() = runTest {
        val fixture = RequestBuilderFixture(Constants.PROVIDER_OPENAI, false)
        val configured = MutableStateFlow<Set<String>?>(null)
        val recommendation = MutableStateFlow<String?>(fixture.modelId)
        every { fixture.settings.subagentEnabledModels } returns configured
        every { fixture.settings.subagentRecommendedModel } returns recommendation
        val unconfigured = fixture.builder.captureAdmissionSnapshot("conversation", "run", fixture.modelId)
        val unconfiguredProjection = fixture.builder.captureContextProjectionSnapshot("conversation", fixture.modelId)
        for (context in listOf(unconfigured.context, unconfiguredProjection.context)) {
            assertEquals(listOf(fixture.modelId), context.enabledModelIds)
            assertEquals(emptyList<String>(), context.subagentModelIds)
            assertNull(context.subagentRecommendedModel)
        }
        configured.value = emptySet()
        val empty = fixture.builder.captureContextProjectionSnapshot("conversation", fixture.modelId)
        assertEquals(emptyList<String>(), empty.context.subagentModelIds)
        assertNull(empty.context.subagentRecommendedModel)
        configured.value = setOf(fixture.modelId)
        recommendation.value = "Disabled:model"
        val invalid = fixture.builder.captureAdmissionSnapshot("conversation", "invalid", fixture.modelId)
        assertEquals(listOf(fixture.modelId), invalid.context.subagentModelIds)
        assertNull(invalid.context.subagentRecommendedModel)
        assertTrue(invalid.context.subagentToolsEnabled)
    }
}
