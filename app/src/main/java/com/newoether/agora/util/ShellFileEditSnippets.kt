package com.newoether.agora.util

/**
 * A numbered excerpt of an edited file around one or more changed regions, so the model can
 * confirm an edit without reading the file again. Mirrors Conch's `snippets` (start_line + lines).
 */
data class ShellFileEditSnippet(
    val startLine: Int,
    val lines: List<String>,
)

internal const val FILE_EDIT_SNIPPET_CONTEXT = 3
internal const val MAX_FILE_EDIT_SNIPPETS = 10
internal const val MAX_FILE_EDIT_SNIPPET_LINES = 60

/** Bounds range bookkeeping for replace_all over many matches. */
private const val MAX_TRACKED_EDIT_RANGES = 1000

/**
 * Follows the char ranges that edits inserted into the content, remapping earlier ranges as later
 * edits shift or overwrite them. Port of Conch's fileEditTracker (handler/file_edit_snippets.go);
 * Kotlin offsets are UTF-16 indices, which does not change any line boundary.
 */
internal class FileEditRangeTracker {
    /** Half-open [start, end) ranges in the current content. */
    private val ranges = mutableListOf<IntArray>()
    private var truncated = false

    /**
     * Records one replacement pass. [matches] are the replaced ranges in the previous content and
     * [inserted] the matching half-open ranges in the new content; both sorted and non-overlapping.
     */
    fun apply(matches: List<IntRange>, inserted: List<IntArray>) {
        val replaced = matches.map { intArrayOf(it.first, it.last + 1) }
        for (range in ranges) {
            val start = remap(range[0], replaced, inserted, leanStart = true)
            val end = remap(range[1], replaced, inserted, leanStart = false)
            range[0] = start
            range[1] = end
        }
        val room = (MAX_TRACKED_EDIT_RANGES - ranges.size).coerceAtLeast(0)
        if (room < inserted.size) truncated = true
        inserted.take(room).forEach { ranges += it.copyOf() }
    }

    /**
     * An offset strictly inside a replaced range snaps to the start (leanStart) or end of its
     * replacement, so a tracked range overlapping a later edit grows to cover it.
     */
    private fun remap(p: Int, matches: List<IntArray>, inserted: List<IntArray>, leanStart: Boolean): Int {
        // First match whose end is beyond p; all earlier matches lie at or before p.
        var lo = 0
        var hi = matches.size
        while (lo < hi) {
            val mid = (lo + hi) ushr 1
            if (matches[mid][1] > p) hi = mid else lo = mid + 1
        }
        val i = lo
        if (i < matches.size && p > matches[i][0]) {
            return if (leanStart) inserted[i][0] else inserted[i][1]
        }
        if (i == 0) return p
        return p + inserted[i - 1][1] - matches[i - 1][1]
    }

    /** Renders the tracked ranges of [content] as merged, numbered excerpts. */
    fun snippets(content: String): Pair<List<ShellFileEditSnippet>, Boolean> {
        if (ranges.isEmpty() || content.isEmpty()) return emptyList<ShellFileEditSnippet>() to truncated
        val sorted = ranges.sortedBy { it[0] }

        // lineStarts[k] is the offset where 1-based line k+1 begins.
        val starts = ArrayList<Int>()
        starts += 0
        content.forEachIndexed { index, char -> if (char == '\n') starts += index + 1 }
        fun lineOf(offset: Int): Int {
            val clamped = offset.coerceIn(0, content.length)
            // Number of line starts at or before the offset.
            var lo = 0
            var hi = starts.size
            while (lo < hi) {
                val mid = (lo + hi) ushr 1
                if (starts[mid] <= clamped) lo = mid + 1 else hi = mid
            }
            return lo
        }
        val totalLines = shellFileLineCount(content)
        fun clampLine(line: Int): Int = when {
            line < 1 -> 1
            totalLines > 0 && line > totalLines -> totalLines
            else -> line
        }

        val windows = mutableListOf<IntArray>()
        for (range in sorted) {
            val end = if (range[1] > range[0]) range[1] - 1 else range[1]
            val first = clampLine(lineOf(range[0]) - FILE_EDIT_SNIPPET_CONTEXT)
            val last = clampLine(lineOf(end) + FILE_EDIT_SNIPPET_CONTEXT)
            val previous = windows.lastOrNull()
            if (previous != null && first <= previous[1] + 1) {
                if (last > previous[1]) previous[1] = last
                continue
            }
            windows += intArrayOf(first, last)
        }

        var anyTruncated = truncated
        if (windows.size > MAX_FILE_EDIT_SNIPPETS) anyTruncated = true
        val out = windows.take(MAX_FILE_EDIT_SNIPPETS).map { window ->
            var last = window[1]
            if (last - window[0] + 1 > MAX_FILE_EDIT_SNIPPET_LINES) {
                last = window[0] + MAX_FILE_EDIT_SNIPPET_LINES - 1
                anyTruncated = true
            }
            val lines = (window[0]..last).mapNotNull { line ->
                val begin = starts.getOrNull(line - 1) ?: return@mapNotNull null
                val stop = starts.getOrNull(line)?.let { it - 1 } ?: content.length
                content.substring(begin, stop).removeSuffix("\r").shellGrepLine()
            }
            ShellFileEditSnippet(startLine = window[0], lines = lines)
        }
        return out to anyTruncated
    }
}

/**
 * Optimistic-concurrency guard shared by the SSH and sandbox backends, matching Conch's wording.
 * [current] is null when the file does not exist.
 */
internal fun shellExpectedSha256Error(expected: String, current: ByteArray?): String? {
    if (expected.isBlank()) return null
    if (current == null) return "verify expected_sha256: file does not exist"
    val actual = shellFileSha256(current)
    return if (actual.equals(expected.trim(), ignoreCase = true)) null
    else "file changed concurrently: expected sha256 ${expected.trim()}, got $actual"
}
