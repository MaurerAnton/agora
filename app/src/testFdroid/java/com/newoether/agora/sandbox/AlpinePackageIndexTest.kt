package com.newoether.agora.sandbox

import java.io.File
import java.util.zip.GZIPOutputStream
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveOutputStream
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class AlpinePackageIndexTest {
    @get:Rule val temporaryFolder = TemporaryFolder()

    @Test
    fun shellProviderUsesRepositoryPriorityRegardlessOfRecordOrder() {
        for (records in listOf(listOf(busybox, yash), listOf(yash, busybox))) {
            val (_, providers) = parseFullApkIndex(index(records.joinToString("\n\n")))
            assertEquals("busybox-binsh", providers["/bin/sh"])
            assertEquals("busybox-binsh", providers["cmd:sh"])
        }
    }

    @Test
    fun recordsEndAtBlankLinesAndFlushAtEofWithoutChecksumFields() {
        val (packages, providers) = parseFullApkIndex(index("""
            P:first
            V:1-r0
            D:dependency
            p:cmd:first=1

            P:last
            V:2-r0
            p:cmd:last=2
        """.trimIndent()))
        assertEquals("1-r0", packages.getValue("first").version)
        assertEquals(listOf("dependency"), packages.getValue("first").deps)
        assertEquals(emptyList<String>(), packages.getValue("last").deps)
        assertEquals("first", providers["cmd:first"])
        assertEquals("last", providers["cmd:last"])
    }

    @Test
    fun packageFieldsAreNotTruncatedAfterThirtyLines() {
        val padding = List(35) { "T:metadata" }.joinToString("\n")
        val (packages, providers) = parseFullApkIndex(index("P:long\nV:1\n$padding\nD:libc\np:cmd:long=1"))
        assertEquals(listOf("libc"), packages.getValue("long").deps)
        assertEquals("long", providers["cmd:long"])
    }

    @Test
    fun installedProviderIsPreservedEvenWhenAnotherHasHigherPriority() {
        for (records in listOf(listOf(busybox, yash), listOf(yash, busybox))) {
            val (_, providers) = parseFullApkIndex(
                index(records.joinToString("\n\n")), preferredPackages = setOf("yash-binsh"),
            )
            assertEquals("yash-binsh", providers["/bin/sh"])
        }
    }

    @Test
    fun equalPriorityUsesStableNameOrder() {
        val first = "P:first\nV:1\np:cmd:tool=1"
        val last = "P:last\nV:1\np:cmd:tool=1"
        for (records in listOf(listOf(first, last), listOf(last, first))) {
            assertEquals("first", parseFullApkIndex(index(records.joinToString("\n\n"))).second["cmd:tool"])
        }
    }

    @Test
    fun installAndUpgradeClosuresDoNotAddAConflictingShellOrDowngradeLibraries() {
        val records = listOf(
            busybox, yash,
            "P:alpine-baselayout\nV:3.6.8-r1\nD:/bin/sh",
            "P:busybox\nV:1.37.0-r14\nD:so:libc.so",
            "P:musl\nV:1.2.5\np:so:libc.so=1",
            "P:python3\nV:3.12\nD:/bin/sh so:libc.so",
            "P:yash\nV:2.57",
        )
        val installed = mapOf("busybox-binsh" to "1.37.0-r13", "busybox" to "1.37.0-r13", "musl" to "1.2.6")
        val (packages, providers) = parseFullApkIndex(
            index(records.joinToString("\n\n")), preferredPackages = installed.keys,
        )
        assertEquals(setOf("python3", "busybox-binsh", "busybox"),
            collectAlpinePackageChanges(listOf("python3"), packages, providers, installed))
        assertEquals(setOf("alpine-baselayout", "busybox-binsh", "busybox"),
            collectAlpinePackageChanges(listOf("alpine-baselayout", "busybox-binsh"), packages, providers, installed))
        val upToDate = packages.mapValues { it.value.version }
        assertEquals(emptySet<String>(),
            collectAlpinePackageChanges(listOf("python3"), packages, providers, upToDate))
    }

    private fun index(contents: String): File = temporaryFolder.newFile().apply {
        GZIPOutputStream(outputStream()).use { gzip ->
            TarArchiveOutputStream(gzip).use { tar ->
                val bytes = contents.toByteArray()
                tar.putArchiveEntry(TarArchiveEntry("APKINDEX").apply { size = bytes.size.toLong() })
                tar.write(bytes)
                tar.closeArchiveEntry()
            }
        }
    }

    private val busybox = "C:checksum\nP:busybox-binsh\nV:1.37.0-r14\nk:100\nD:busybox=1.37.0-r14\np:/bin/sh cmd:sh=1.37.0-r14"
    private val yash = "C:checksum\nP:yash-binsh\nV:2.57-r0\nk:50\nD:yash=2.57-r0\np:/bin/sh cmd:sh=2.57-r0"
}
