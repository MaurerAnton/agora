package com.newoether.agora.data

import android.content.Context
import io.mockk.every
import io.mockk.mockk
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class MemoryManagerFoldersTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    private fun manager(): Pair<MemoryManager, File> {
        val root = temporaryFolder.newFolder("memory")
        val context = mockk<Context> {
            every { filesDir } returns root
        }
        return MemoryManager(context) to File(root, "memory_db")
    }

    @Test
    fun nestedCreateReadListKeepsRelativePaths() {
        val (manager, db) = manager()
        manager.createFile("work/project/note", "body", "desc")
        assertEquals("body", manager.readFile("work/project/note.md"))
        assertEquals("desc", manager.getDescription("work/project/note"))
        assertEquals(
            listOf("work/project/note.md"),
            manager.listFiles().map { it.name },
        )
        assertTrue(File(db, "work/project/note.md").exists())
    }

    @Test
    fun moveAcrossFoldersKeepsMetadataAndPrunesEmptyParents() {
        val (manager, db) = manager()
        manager.createFile("a/note", "body", "desc")
        manager.editFile(name = "a/note.md", newName = "b/c/note.md")
        assertFalse(File(db, "a/note.md").exists())
        assertFalse(File(db, "a").exists())
        assertEquals("body", manager.readFile("b/c/note"))
        assertEquals("desc", manager.getDescription("b/c/note.md"))
    }

    @Test
    fun deletePrunesEmptyParents() {
        val (manager, db) = manager()
        manager.createFile("a/b/note", "body")
        manager.createFile("a/keep", "body")
        manager.deleteFile("a/b/note.md")
        assertFalse(File(db, "a/b").exists())
        assertTrue(File(db, "a/keep.md").exists())
    }

    @Test
    fun traversalIsRejected() {
        val (manager, _) = manager()
        assertThrows(IllegalArgumentException::class.java) {
            manager.createFile("../evil", "body")
        }
        assertThrows(IllegalArgumentException::class.java) {
            manager.createFile("a/../../evil", "body")
        }
        assertThrows(IllegalArgumentException::class.java) {
            manager.createFile("", "body")
        }
    }

    @Test
    fun rootBehaviorUnchanged() {
        val (manager, _) = manager()
        manager.createFile("note", "body", "desc")
        assertEquals(listOf("note.md"), manager.listFiles().map { it.name })
        assertEquals("desc", manager.getDescription("note.md"))
    }
}
