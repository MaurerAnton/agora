package com.newoether.agora.util

/** One replacement inside a file_edit batch. */
data class ShellFileEdit(
    val oldString: String,
    val newString: String,
    val replaceAll: Boolean = false,
)

internal const val SHELL_FILE_EDIT_MAX_OPERATIONS = 100
internal const val SHELL_FILE_GREP_MAX_CONTEXT_LINES = 10

/** Version-control metadata directories that file search never descends into. */
internal val SHELL_FILE_SEARCH_SKIPPED_DIRS = setOf(".git", ".hg", ".svn")

data class ShellGrepOptions(
    val caseInsensitive: Boolean = false,
    /** Lines of context on each side of a match, already clamped to 0..10. */
    val context: Int = 0,
)

data class ShellGrepResult(
    val matches: List<ShellClient.GrepMatch>,
    val truncated: Boolean,
    /** Candidate files skipped for exceeding the 500 KB grep cap. */
    val skippedLargeFiles: Int = 0,
    /** False when the backend silently ignored a context request (older Conch servers). */
    val contextSupported: Boolean = true,
)

/**
 * Decodes a raw file slice the same way Conch does: drop at most three leading continuation bytes
 * (the offset landed inside a rune) and, unless the slice reaches end of file, a trailing rune
 * prefix the read cut short. Invalid bytes are never trimmed, so paging by the returned byte count
 * cannot skip data or stall. Returns the decoded text and the raw bytes consumed.
 */
internal fun decodeUtf8FileSlice(
    bytes: ByteArray,
    length: Int,
    reachesEndOfFile: Boolean,
): Pair<String, Int> {
    var start = 0
    while (start < 3 && start < length && bytes[start].toInt() and 0xc0 == 0x80) start += 1
    var end = length
    if (!reachesEndOfFile) {
        for (k in 1..minOf(3, end - start)) {
            val index = end - k
            val lead = bytes[index].toInt() and 0xff
            if (lead and 0xc0 == 0x80) continue
            val expected = when {
                lead and 0xe0 == 0xc0 -> 2
                lead and 0xf0 == 0xe0 -> 3
                lead and 0xf8 == 0xf0 -> 4
                else -> 1
            }
            if (k < expected) end = index
            break
        }
    }
    return String(bytes, start, end - start, Charsets.UTF_8) to end
}

/** Caps a returned grep line without splitting a surrogate pair. */
internal fun String.shellGrepLine(): String {
    if (length <= SHELL_FILE_GREP_MAX_CONTENT_CHARS) return this
    var cut = SHELL_FILE_GREP_MAX_CONTENT_CHARS
    if (this[cut - 1].isHighSurrogate()) cut -= 1
    return substring(0, cut)
}

/**
 * Appends matches from one file's text until [matches] reaches [limit]. Lines split on LF with a
 * trailing CR stripped and no phantom line after a final newline, matching Conch line numbers.
 */
internal fun grepShellFileText(
    path: String,
    text: String,
    regex: Regex,
    context: Int,
    limit: Int,
    matches: MutableList<ShellClient.GrepMatch>,
) {
    val lines = text.split('\n').toMutableList()
    if (lines.isNotEmpty() && lines.last().isEmpty()) lines.removeAt(lines.lastIndex)
    for (i in lines.indices) lines[i] = lines[i].removeSuffix("\r")
    for ((index, line) in lines.withIndex()) {
        if (!regex.containsMatchIn(line)) continue
        matches += ShellClient.GrepMatch(
            path = path,
            line = index + 1,
            content = line.shellGrepLine(),
            before = if (context > 0) {
                lines.subList(maxOf(0, index - context), index).map { it.shellGrepLine() }
            } else emptyList(),
            after = if (context > 0) {
                lines.subList(index + 1, minOf(lines.size, index + 1 + context)).map { it.shellGrepLine() }
            } else emptyList(),
        )
        if (matches.size >= limit) return
    }
}
