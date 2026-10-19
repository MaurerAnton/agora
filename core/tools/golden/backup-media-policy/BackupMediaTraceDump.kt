package backup.policy.golden

import com.newoether.agora.data.NativeBackupMediaPolicy
import com.newoether.agora.model.AttachmentImportState
import com.newoether.agora.model.AttachmentStorage
import com.newoether.agora.model.SelectedAttachment
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.builtins.serializer
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import java.io.File

/**
 * Stand driver for data/NativeBackupMediaPolicy.kt, which compiles verbatim from the app tree.
 *
 * The policy is pure: its inputs are lists of model data classes and its only collaborators are
 * four callbacks. Everything observable therefore fits in one trace: the produced value, that
 * value after kotlinx.serialization, and -- crucially -- the exact sequence of callback
 * invocations. firstNotNullOfOrNull short-circuits, so a port that probes candidates in a
 * different order would return the same result while asking the archive layer for the wrong
 * things; the call log is what makes that difference visible.
 */
private val encoder = Json { ignoreUnknownKeys = true }
private val attachmentList = ListSerializer(SelectedAttachment.serializer())
private val stringList = ListSerializer(String.serializer())

// ---------------------------------------------------------------------------------------------
// Trace escaping
// ---------------------------------------------------------------------------------------------

private fun esc(value: String): String {
    val out = StringBuilder(value.length)
    for (c in value) {
        when (c) {
            '\\' -> out.append("\\\\")
            '|' -> out.append("\\p")
            ';' -> out.append("\\s")
            '\n' -> out.append("\\n")
            '\r' -> out.append("\\r")
            '\t' -> out.append("\\t")
            else -> out.append(c)
        }
    }
    return out.toString()
}

/** Renders a nullable callback result; `-` means "resolved to null". */
private fun opt(value: String?): String = if (value == null) "-" else esc(value)

// ---------------------------------------------------------------------------------------------
// Scenario decoding
// ---------------------------------------------------------------------------------------------

private fun JsonObject.str(key: String): String? {
    val element = this[key]
    if (element == null || element is JsonNull) return null
    val primitive = element as? JsonPrimitive ?: return null
    return primitive.content
}

private fun JsonObject.requiredStr(key: String): String =
    str(key) ?: error("scenario field `$key` is required")

private fun JsonObject.int(key: String): Int? =
    (this[key] as? JsonPrimitive)?.takeIf { it !is JsonNull }?.intOrNull

private fun JsonObject.long(key: String): Long? =
    (this[key] as? JsonPrimitive)?.takeIf { it !is JsonNull }?.longOrNull

private fun JsonObject.bool(key: String, fallback: Boolean): Boolean =
    (this[key] as? JsonPrimitive)?.takeIf { it !is JsonNull }?.booleanOrNull ?: fallback

private fun JsonObject.strings(key: String): List<String>? =
    (this[key] as? JsonArray)?.map { it.jsonPrimitive.content }

private fun JsonObject.intSet(key: String): Set<Int>? =
    (this[key] as? JsonArray)
        ?.map { it.jsonPrimitive.content.toInt() }
        ?.toCollection(LinkedHashSet())

private fun storageOf(name: String?): AttachmentStorage = when (name) {
    null, "app_private" -> AttachmentStorage.APP_PRIVATE
    "local_sandbox_pending" -> AttachmentStorage.LOCAL_SANDBOX_PENDING
    "local_sandbox_runtime" -> AttachmentStorage.LOCAL_SANDBOX_RUNTIME
    else -> error("unknown attachment storage `$name`")
}

private fun importStateOf(name: String?): AttachmentImportState = when (name) {
    null, "ready" -> AttachmentImportState.READY
    "processing" -> AttachmentImportState.PROCESSING
    "failed" -> AttachmentImportState.FAILED
    else -> error("unknown import state `$name`")
}

private fun JsonObject.attachment(): SelectedAttachment = SelectedAttachment(
    localId = requiredStr("localId"),
    uri = requiredStr("uri"),
    type = requiredStr("type"),
    frameCount = int("frameCount"),
    sliceIntervalMs = long("sliceIntervalMs"),
    pageCount = int("pageCount"),
    videoDurationMs = long("videoDurationMs"),
    fileName = str("fileName"),
    mimeType = str("mimeType"),
    fileSize = long("fileSize"),
    pixelWidth = int("pixelWidth"),
    pixelHeight = int("pixelHeight"),
    processedFrames = strings("processedFrames"),
    selectedPages = intSet("selectedPages"),
    preRenderedPaths = strings("preRenderedPaths"),
    localPath = str("localPath"),
    storage = storageOf(str("storage")),
    sandboxPath = str("sandboxPath"),
    importState = importStateOf(str("importState")),
    preparedText = str("preparedText"),
    unavailable = bool("unavailable", false),
)

private fun JsonObject.attachments(): List<SelectedAttachment> =
    (this["attachments"] as? JsonArray)?.map { it.jsonObject.attachment() } ?: emptyList()

/** The raw column under test: either a literal string (possibly malformed) or a JSON value. */
private fun JsonObject.raw(): String? {
    this["raw_json"]?.let { return if (it is JsonNull) null else it.toString() }
    return str("raw")
}

private fun JsonObject.originalImageSources(): List<String> =
    strings("originalImageSources") ?: emptyList()

private fun JsonObject.indexMap(key: String): Map<Int, Int> {
    val obj = this[key] as? JsonObject ?: return emptyMap()
    return obj.entries.associate { (name, value) ->
        name.toInt() to value.jsonPrimitive.content.toInt()
    }
}

private fun JsonObject.stringMap(key: String): Map<Int, String> {
    val obj = this[key] as? JsonObject ?: return emptyMap()
    return obj.entries.associate { (name, value) -> name.toInt() to value.jsonPrimitive.content }
}

private fun JsonObject.stringTable(callback: String): Map<String, String?> {
    val obj = (this["table"] as? JsonObject)?.get(callback) as? JsonObject ?: return emptyMap()
    return obj.entries.associate { (key, value) ->
        key to if (value is JsonNull) null else value.jsonPrimitive.content
    }
}

/** The primary-restoration callback returns a Pair, so its table values are two-element arrays. */
private fun JsonObject.pairTable(callback: String): Map<String, Pair<String, String>?> {
    val obj = (this["table"] as? JsonObject)?.get(callback) as? JsonObject ?: return emptyMap()
    return obj.entries.associate { (key, value) ->
        if (value is JsonNull) {
            key to null
        } else {
            val pair = (value as JsonArray).map { it.jsonPrimitive.content }
            require(pair.size == 2) { "restoredPrimaryForArchiveEntry table entry `$key` needs 2 elements" }
            key to (pair[0] to pair[1])
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------------------------

private class Trace {
    private val lines = mutableListOf<String>()
    var missing = 0
        private set

    fun head(name: String, fn: String) {
        lines += "name|${esc(name)}"
        lines += "fn|${esc(fn)}"
    }

    fun call(callback: String, arg: String, result: String?) {
        lines += "call|${esc(callback)}|${esc(arg)}|${opt(result)}"
    }

    fun callPair(callback: String, arg: String, result: Pair<String, String>?) {
        lines += "call|${esc(callback)}|${esc(arg)}|${opt(result?.let { it.first + "|" + it.second })}"
    }

    fun missingResource() {
        missing += 1
    }

    fun attachments(value: List<SelectedAttachment>) {
        lines += "result|attachments|${esc(encoder.encodeToString(attachmentList, value))}"
    }

    fun strings(value: List<String>) {
        lines += "result|strings|${esc(encoder.encodeToString(stringList, value))}"
    }

    fun string(value: String?) {
        lines += "result|string|${opt(value)}"
    }

    fun render(): String = lines.joinToString("\n") + "\nmissing|$missing\n"
}

private fun run(root: JsonObject, trace: Trace) {
    val attachments = root.attachments()
    val archiveEntry = root.stringTable("archiveEntryForSource")
    val restoredPath = root.stringTable("restoredPathForArchiveEntry")
    val restoredUri = root.stringTable("restoredUriForArchiveEntry")
    val restoredPrimary = root.pairTable("restoredPrimaryForArchiveEntry")

    when (val fn = root.requiredStr("fn")) {
        "exportableDraftAttachments" ->
            trace.attachments(NativeBackupMediaPolicy.exportableDraftAttachments(attachments))

        "rewriteDraftAttachmentsForExport" -> trace.attachments(
            NativeBackupMediaPolicy.rewriteDraftAttachmentsForExport(
                attachments = attachments,
                archiveEntryForSource = { source ->
                    val entry = archiveEntry[source]
                    trace.call("archiveEntryForSource", source, entry)
                    entry
                },
                onMissingResource = { trace.missingResource() },
            ),
        )

        "restoreDraftAttachments" -> trace.attachments(
            NativeBackupMediaPolicy.restoreDraftAttachments(
                attachments = attachments,
                restoredPrimaryForArchiveEntry = { entry ->
                    val restored = restoredPrimary[entry]
                    trace.callPair("restoredPrimaryForArchiveEntry", entry, restored)
                    restored
                },
                restoredPathForArchiveEntry = { entry ->
                    val path = restoredPath[entry]
                    trace.call("restoredPathForArchiveEntry", entry, path)
                    path
                },
            ),
        )

        "toolImagePaths" -> trace.strings(NativeBackupMediaPolicy.toolImagePaths(root.raw()))

        "rewriteAttachmentMetaForExport" -> trace.string(
            NativeBackupMediaPolicy.rewriteAttachmentMetaForExport(
                raw = root.raw(),
                originalImageSources = root.originalImageSources(),
                oldToNewImageIndex = root.indexMap("oldToNewImageIndex"),
                archiveEntryForSource = { source ->
                    val entry = archiveEntry[source]
                    trace.call("archiveEntryForSource", source, entry)
                    entry
                },
                onMissingResource = { trace.missingResource() },
            ),
        )

        "rewriteToolImagePathsForExport" -> trace.string(
            NativeBackupMediaPolicy.rewriteToolImagePathsForExport(
                raw = root.raw(),
                archiveEntryForSource = { source ->
                    val entry = archiveEntry[source]
                    trace.call("archiveEntryForSource", source, entry)
                    entry
                },
            ),
        )

        "restoreAttachmentMeta" -> trace.string(
            NativeBackupMediaPolicy.restoreAttachmentMeta(
                raw = root.raw(),
                archiveVersion = root.int("archiveVersion") ?: 4,
                oldToNewImageIndex = root.indexMap("oldToNewImageIndex"),
                legacyVideoUris = root.stringMap("legacyVideoUris"),
                restoredUriForArchiveEntry = { entry ->
                    val uri = restoredUri[entry]
                    trace.call("restoredUriForArchiveEntry", entry, uri)
                    uri
                },
            ),
        )

        "restoreToolImagePaths" -> trace.string(
            NativeBackupMediaPolicy.restoreToolImagePaths(
                raw = root.raw(),
                archiveVersion = root.int("archiveVersion") ?: 4,
                restoredPathForArchiveEntry = { entry ->
                    val path = restoredPath[entry]
                    trace.call("restoredPathForArchiveEntry", entry, path)
                    path
                },
            ),
        )

        else -> error("unknown function `$fn`")
    }
}

fun main(args: Array<String>) {
    val scenario = File(args[0])
    val root = Json.parseToJsonElement(scenario.readText()).jsonObject
    val trace = Trace()
    trace.head(root.str("name") ?: scenario.nameWithoutExtension, root.requiredStr("fn"))
    run(root, trace)
    print(trace.render())
}