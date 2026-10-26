// Golden trace driver for the sandbox path resolver and file operations in
// app/src/fdroid/java/com/newoether/agora/sandbox/SandboxPathResolver.kt and
// SandboxFileOperations.kt.
package sandbox.path.resolver.golden

import com.newoether.agora.sandbox.SandboxPathResolver
import com.newoether.agora.sandbox.fileRead
import com.newoether.agora.sandbox.fileWrite
import com.newoether.agora.sandbox.fileGlob
import com.newoether.agora.sandbox.fileGrep
import com.newoether.agora.sandbox.fileEdit
import com.newoether.agora.sandbox.walkVirtualFiles
import com.newoether.agora.sandbox.globMatch
import com.newoether.agora.util.ShellFileReadResult
import com.newoether.agora.util.ShellFileEditResult
import com.newoether.agora.util.shellFileLineCount
import com.newoether.agora.util.shellFileSha256
import kotlinx.coroutines.runBlocking
import java.io.File
import java.nio.charset.StandardCharsets
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json { prettyPrint = true }

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        val scenarioDir = File(args[2])
        // Create test directory structure
        val testRoot = File(outputDir, "test_root")
        setupTestDirectory(testRoot)
        for (path in args.drop(3)) {
            val scenario = File(path)
            File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario, testRoot) + "\n")
        }
        return
    }
    println(render(File(args[0]), File(args[1])))
}

private fun setupTestDirectory(root: File) {
    root.deleteRecursively()
    root.mkdirs()
    // Create test files
    File(root, "test.txt").writeText("Hello World", StandardCharsets.UTF_8)
    File(root, "large.txt").writeText("A".repeat(10000), StandardCharsets.UTF_8)
    File(root, "dir").mkdirs()
    File(root, "dir/c.txt").writeText("content c", StandardCharsets.UTF_8)
    File(root, "dir/sub").mkdirs()
    File(root, "dir/sub/d.txt").writeText("content d", StandardCharsets.UTF_8)
    File(root, "other").mkdirs()
    File(root, "other/e.txt").writeText("content e", StandardCharsets.UTF_8)
    // Home mount directory
    File(root, "home").mkdirs()
    File(root, "home/a.txt").writeText("home a", StandardCharsets.UTF_8)
    File(root, "home/dir").mkdirs()
    File(root, "home/dir/b.txt").writeText("home b", StandardCharsets.UTF_8)
    // Shared storage directory
    File(root, "shared").mkdirs()
    File(root, "shared/a.txt").writeText("shared a", StandardCharsets.UTF_8)
    File(root, "shared/dir").mkdirs()
    File(root, "shared/dir/b.txt").writeText("shared b", StandardCharsets.UTF_8)
}

private fun render(scenarioFile: File, testRoot: File): String {
    val scenario = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val op = scenario.string("op")
    val homeMountPath = "/mnt/home"
    val sharedMountPath = "/mnt/shared"
    val homeMountDir = File(testRoot, "home").canonicalFile
    val sharedStorageDir = File(testRoot, "shared").canonicalFile
    val rootfsDir = testRoot.canonicalFile
    
    val resolver = SandboxPathResolver(
        rootfsDir = rootfsDir,
        homeMountDir = homeMountDir,
        homeMountPath = homeMountPath,
        sharedStorageMountPath = sharedMountPath,
        sharedStorageHostDir = { sharedStorageDir }
    )
    
    return buildJsonObject {
        put("op", op)
        when (op) {
            "resolve" -> {
                val path = scenario.string("path")
                put("path", path)
                put("home_mount_path", scenario.string("home_mount_path"))
                put("shared_storage_mount_path", scenario.string("shared_storage_mount_path"))
                try {
                    val resolved = resolver.resolveSandboxPath(path)
                    put("outcome", buildJsonObject {
                        put("file", resolved.file.canonicalPath)
                        put("physical_root", resolved.physicalRoot.canonicalPath)
                        put("virtual_root", resolved.virtualRoot)
                    })
                } catch (e: Exception) {
                    put("outcome", buildJsonObject {
                        put("error", e.message ?: e.javaClass.simpleName)
                    })
                }
            }
            "walk" -> {
                val files = scenario.strings("files")
                val physicalRoot = scenario.string("physical_root")
                val virtualRoot = scenario.string("virtual_root")
                val remaining = scenario.intOrNull("remaining") ?: -1
                put("files", stringsJson(files))
                put("physical_root", physicalRoot)
                put("virtual_root", virtualRoot)
                put("remaining", remaining)
                val result = mutableListOf<String>()
                val dir = File(physicalRoot)
                if (dir.exists()) {
                    walkVirtualFiles(dir, result, physicalRoot, virtualRoot, remaining)
                }
                put("outcome", buildJsonObject {
                    put("files", stringsJson(result))
                })
            }
            "file_read" -> {
                val path = scenario.string("path")
                val offset = scenario.longOrNull("offset") ?: 0L
                val limit = scenario.longOrNull("limit") ?: 0L
                put("path", path)
                put("offset", offset)
                put("limit", limit)
                try {
                    val result = runBlocking { fileRead(resolver, path, offset, limit) }
                    put("outcome", buildJsonObject {
                        put("content", result.content)
                        put("lines", result.lines)
                        put("total_lines", result.totalLines)
                        put("total_bytes", result.totalBytes)
                        put("returned_bytes", result.returnedBytes)
                        put("offset", result.offset)
                        put("limit", result.limit)
                        put("truncated", result.truncated)
                        result.error?.let { put("error", it) }
                    })
                } catch (e: Exception) {
                    put("outcome", buildJsonObject {
                        put("error", e.message ?: e.javaClass.simpleName)
                    })
                }
            }
            else -> error("unknown op $op")
        }
    }.toString()
}

private fun stringsJson(values: List<String>): JsonArray = buildJsonArray {
    for (value in values) add(value)
}

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.strings(name: String): List<String> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { it.jsonPrimitive.content }
}

private fun JsonObject.longOrNull(name: String): Long? =
    (this[name] as? JsonPrimitive)?.content?.toLongOrNull()

private fun JsonObject.intOrNull(name: String): Int? =
    (this[name] as? JsonPrimitive)?.content?.toIntOrNull()