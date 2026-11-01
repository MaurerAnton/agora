package com.newoether.agora.data

import android.content.Context
import com.newoether.agora.util.SecretCrypto
import io.mockk.every
import io.mockk.mockk
import io.mockk.mockkObject
import io.mockk.mockkStatic
import io.mockk.unmockkObject
import io.mockk.unmockkStatic
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class SubagentCapsuleDisplaySettingsTest {
    @Test
    fun defaultsAndInvalidValuesUseGroupedWhileExplicitChoicesRemainValid() {
        assertEquals(SubagentCapsuleDisplayModes.GROUPED, SubagentCapsuleDisplayModes.DEFAULT)
        for (value in listOf(null, "", "unknown", "GROUPED")) {
            assertEquals(SubagentCapsuleDisplayModes.GROUPED, SubagentCapsuleDisplayModes.normalize(value))
        }
        for (value in listOf("individual", "grouped")) {
            assertEquals(value, SubagentCapsuleDisplayModes.normalize(value))
        }
    }

    @Test
    fun displayModePersistsExportsAndRestoresWithoutChangingDelegation() = runTest {
        val directory = Files.createTempDirectory("agora-subagent-capsules").toFile()
        val context = mockk<Context>()
        every { context.applicationContext } returns context
        every { context.filesDir } returns directory
        val movesClass = "androidx.datastore.core.FileMoves_androidKt"
        val atomicMove = Class.forName(movesClass)
            .getDeclaredMethod("atomicMoveTo", File::class.java, File::class.java)
        mockkStatic(movesClass)
        every { atomicMove.invoke(null, any<File>(), any<File>()) } answers {
            Files.move(firstArg<File>().toPath(), secondArg<File>().toPath(), StandardCopyOption.REPLACE_EXISTING)
            true
        }
        mockkObject(SecretCrypto)
        every { SecretCrypto.encrypt(any()) } answers { firstArg<String>() }
        every { SecretCrypto.decrypt(any()) } answers { firstArg<String>() }
        try {
            val manager = SettingsManager(context)
            assertEquals("grouped", manager.subagentCapsuleDisplayMode.first())
            manager.saveSubagentToolsEnabled(false)
            manager.saveSubagentCapsuleDisplayMode("individual")
            assertEquals("individual", SettingsManager(context).subagentCapsuleDisplayMode.first())
            assertFalse(manager.subagentToolsEnabled.first())
            val exported = PortableSettingsArchive.toJsonObject(manager, false)
            assertEquals(JsonPrimitive("individual"), exported["subagentCapsuleDisplayMode"])
            val empty = JsonObject(emptyMap())
            PortableSettingsArchive.restoreFromJsonObject(empty, manager, false, false, null) { it }
            assertEquals("individual", manager.subagentCapsuleDisplayMode.first())
            PortableSettingsArchive.restoreFromJsonObject(empty, manager, true, false, null) { it }
            assertEquals("grouped", manager.subagentCapsuleDisplayMode.first())
            val grouped = JsonObject(mapOf("subagentCapsuleDisplayMode" to JsonPrimitive("grouped")))
            PortableSettingsArchive.restoreFromJsonObject(grouped, manager, false, false, null) { it }
            assertEquals("grouped", manager.subagentCapsuleDisplayMode.first())
            manager.saveSubagentCapsuleDisplayMode("future")
            assertEquals("grouped", manager.subagentCapsuleDisplayMode.first())
            PortableSettingsArchive.restoreFromJsonObject(grouped, manager, true, false, null) { it }
            assertEquals("grouped", SettingsManager(context).subagentCapsuleDisplayMode.first())
            val individual = JsonObject(mapOf("subagentCapsuleDisplayMode" to JsonPrimitive("individual")))
            PortableSettingsArchive.restoreFromJsonObject(individual, manager, true, false, null) { it }
            assertEquals("individual", SettingsManager(context).subagentCapsuleDisplayMode.first())
            PortableSettingsArchive.restoreFromJsonObject(
                JsonObject(mapOf("subagentCapsuleDisplayMode" to JsonPrimitive("future"))),
                manager, false, false, null,
            ) { it }
            assertEquals("grouped", manager.subagentCapsuleDisplayMode.first())
        } finally {
            unmockkObject(SecretCrypto)
            unmockkStatic(movesClass)
            directory.deleteRecursively()
        }
    }
}
