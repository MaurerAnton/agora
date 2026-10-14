package com.newoether.agora.sandbox

import android.content.Context
import android.system.Os
import com.newoether.agora.data.repository.SettingsRepository
import io.mockk.Runs
import io.mockk.every
import io.mockk.just
import io.mockk.mockk
import io.mockk.mockkStatic
import io.mockk.unmockkStatic
import io.mockk.verify
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import kotlinx.coroutines.runBlocking
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.archivers.tar.TarArchiveOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ProotSandboxSupportTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun alpineVersionsCompareNumericTokensPrereleasesAndRevisions() {
        assertTrue(compareAlpineVersions("1.10-r0", "1.2-r9") > 0)
        assertTrue(compareAlpineVersions("3.5.2-r1", "3.5.2-r0") > 0)
        assertTrue(compareAlpineVersions("2.0_rc1-r0", "2.0-r0") < 0)
        assertEquals(0, compareAlpineVersions("3.5.2-r1", "3.5.2-r1"))
    }

    @Test
    fun virtualPathsNormalizeAndPortableGlobMatchesRemainStable() {
        assertEquals("/", normalizeVirtualPath("  "))
        assertEquals("/home/agora/file.txt", normalizeVirtualPath("home//agora/file.txt/"))
        assertEquals(
            listOf("/home/agora/readme.md"),
            globMatch(
                files = listOf("/home/agora/readme.md", "/home/agora/image.png"),
                pattern = "*.md",
            ),
        )
    }

    @Test
    fun pathResolverKeepsEachVirtualMountInsideItsPhysicalRoot() {
        val rootfs = temporaryFolder.newFolder("rootfs")
        val home = temporaryFolder.newFolder("home")
        val shared = temporaryFolder.newFolder("shared")
        val resolver = SandboxPathResolver(
            rootfsDir = rootfs,
            homeMountDir = home,
            homeMountPath = "/home/agora",
            sharedStorageMountPath = "/mnt/shared",
            sharedStorageHostDir = { shared },
        )

        assertEquals(File(home, "note.txt").canonicalFile, resolver.resolvePath("/home/agora/note.txt"))
        assertEquals(File(shared, "photo.png").canonicalFile, resolver.resolvePath("/mnt/shared/photo.png"))
        assertEquals(File(rootfs, "etc/hosts").canonicalFile, resolver.resolvePath("/etc/hosts"))

        val escaped = runCatching { resolver.resolvePath("/home/agora/../../escape") }
        assertTrue(escaped.exceptionOrNull() is IllegalArgumentException)
    }

    @Test
    fun packageMetadataReadsInstalledVersionsAndRejectsInvalidNames() {
        val rootfs = temporaryFolder.newFolder("metadata-rootfs")
        val installed = File(rootfs, "lib/apk/db/installed")
        installed.parentFile!!.mkdirs()
        installed.writeText("P:busybox\nV:1.36.1-r2\n\nP:apk-tools\nV:2.14.4-r0\n")
        val world = File(rootfs, "etc/apk/world")
        world.parentFile!!.mkdirs()
        world.writeText("busybox\ncurl\n")
        val store = AlpinePackageMetadataStore(rootfs)

        assertEquals(
            linkedMapOf("busybox" to "1.36.1-r2", "apk-tools" to "2.14.4-r0"),
            store.readInstalledVersions(),
        )
        assertTrue(store.isBasePackage("busybox"))
        assertFalse(store.isBasePackage("curl"))
        assertEquals("python3", store.sanitizePackageName(" python3 "))
        assertTrue(runCatching { store.sanitizePackageName("python; rm") }.isFailure)
    }

    @Test
    fun rootfsArchiveExtractionRejectsTraversalEntries() {
        val tarBytes = ByteArrayOutputStream().use { bytes ->
            TarArchiveOutputStream(bytes).use { tar ->
                writeEntry(tar, "safe.txt", "safe")
                writeEntry(tar, "../escape.txt", "escape")
            }
            bytes.toByteArray()
        }
        val destination = temporaryFolder.newFolder("archive-root")
        TarArchiveInputStream(ByteArrayInputStream(tarBytes)).use { tar ->
            extractTarEntries(tar, destination)
        }

        assertEquals("safe", File(destination, "safe.txt").readText())
        assertFalse(File(destination.parentFile, "escape.txt").exists())
    }

    @Test
    fun localSandboxFileSearchMatchesConchBaseline() = runBlocking {
        val filesDir = Files.createTempDirectory("agora-shell-search").toFile()
        val context = mockk<Context>()
        every { context.filesDir } returns filesDir
        val manager = ProotSandboxManager(
            context,
            mockk<SettingsRepository>(relaxed = true),
        )
        try {
            val home = File(filesDir, "sandbox-home").apply { mkdirs() }
            val rootfs = File(filesDir, "alpine-rootfs").apply { mkdirs() }
            File(rootfs, "outside.txt").writeText("outside", Charsets.UTF_8)
            repeat(1_001) { index ->
                File(home, "file-$index.txt").writeText("home", Charsets.UTF_8)
            }

            val (files, globTruncated) = manager.fileGlob("*.txt", "", null)
            assertEquals(1_000, files.size)
            assertTrue(globTruncated)
            assertTrue(files.all { it.startsWith("/home/agora/") })
            assertTrue(manager.fileGrep("[", "", "").isFailure)

            File(home, "matches.log").writeText(
                buildString {
                    repeat(501) {
                        append("needle")
                        append("x".repeat(600))
                        append('\n')
                    }
                },
                Charsets.UTF_8,
            )
            val (matches, grepTruncated) =
                manager.fileGrep("needle", "", "matches.log").getOrThrow()
            assertEquals(500, matches.size)
            assertTrue(grepTruncated)
            assertEquals(500, matches.first().content.length)

            File(home, "oversized.log").writeText(
                "needle" + "x".repeat(512_000),
                Charsets.UTF_8,
            )
            val (oversizedMatches, oversizedTruncated) =
                manager.fileGrep("needle", "", "oversized.log").getOrThrow()
            assertTrue(oversizedMatches.isEmpty())
            assertFalse(oversizedTruncated)
        } finally {
            filesDir.deleteRecursively()
        }
    }

    @Test
    fun localSandboxFileWritePublishesCompleteContentByAtomicRename() = runBlocking {
        val filesDir = Files.createTempDirectory("agora-shell-write").toFile()
        val context = mockk<Context>()
        every { context.filesDir } returns filesDir
        val manager = ProotSandboxManager(
            context,
            mockk<SettingsRepository>(relaxed = true),
        )
        mockkStatic(Os::class)
        every { Os.chmod(any(), any()) } just Runs
        every { Os.rename(any(), any()) } answers {
            val source = File(args[0] as String)
            val target = File(args[1] as String)
            assertEquals(
                requireNotNull(source.parentFile).canonicalPath,
                requireNotNull(target.parentFile).canonicalPath,
            )
            assertEquals("complete content", source.readText(Charsets.UTF_8))
            Files.move(
                source.toPath(),
                target.toPath(),
                StandardCopyOption.REPLACE_EXISTING,
            )
            Unit
        }
        try {
            val target = File(filesDir, "sandbox-home/nested/file.txt")
            val error = manager.fileWrite(
                "/home/agora/nested/file.txt",
                "complete content",
            )

            assertNull(error)
            assertEquals("complete content", target.readText(Charsets.UTF_8))
            verify(exactly = 1) { Os.chmod(any(), 0x1A4) }
            verify(exactly = 1) {
                Os.rename(any(), match { File(it).canonicalPath == target.canonicalPath })
            }
        } finally {
            unmockkStatic(Os::class)
            filesDir.deleteRecursively()
        }
    }

    @Test
    fun localSandboxFileWritePreservesExistingMode() = runBlocking {
        val filesDir = Files.createTempDirectory("agora-shell-write-mode").toFile()
        val context = mockk<Context>()
        every { context.filesDir } returns filesDir
        val manager = ProotSandboxManager(
            context,
            mockk<SettingsRepository>(relaxed = true),
        )
        val target = File(filesDir, "sandbox-home/existing.txt").apply {
            parentFile?.mkdirs()
            writeText("old content", Charsets.UTF_8)
        }
        val existingStat = android.system.StructStat(
            0L,
            0L,
            0x81A0,
            1L,
            0,
            0,
            0L,
            target.length(),
            0L,
            0L,
            0L,
            4_096L,
            0L,
        ).also { stat ->
            android.system.StructStat::class.java.getDeclaredField("st_mode").apply {
                isAccessible = true
                setInt(stat, 0x81A0)
            }
        }
        mockkStatic(Os::class)
        every { Os.stat(any()) } answers {
            assertEquals(target.canonicalPath, File(args[0] as String).canonicalPath)
            existingStat
        }
        every { Os.chmod(any(), any()) } just Runs
        every { Os.rename(any(), any()) } answers {
            Files.move(
                File(args[0] as String).toPath(),
                File(args[1] as String).toPath(),
                StandardCopyOption.REPLACE_EXISTING,
            )
            Unit
        }
        try {
            val error = manager.fileWrite(
                "/home/agora/existing.txt",
                "updated content",
            )

            assertNull(error)
            assertEquals("updated content", target.readText(Charsets.UTF_8))
            verify(exactly = 1) { Os.chmod(any(), 0x1A0) }
        } finally {
            unmockkStatic(Os::class)
            filesDir.deleteRecursively()
        }
    }

    private fun writeEntry(
        tar: TarArchiveOutputStream,
        name: String,
        content: String,
    ) {
        val payload = content.toByteArray()
        tar.putArchiveEntry(TarArchiveEntry(name).apply { size = payload.size.toLong() })
        tar.write(payload)
        tar.closeArchiveEntry()
    }
}
