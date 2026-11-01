package com.newoether.agora.sandbox

import com.newoether.agora.util.PortableGlobMatcher

internal fun normalizeVirtualPath(path: String): String {
    val raw = path.trim().replace('\\', '/')
    val absolute = if (raw.isBlank()) "/" else if (raw.startsWith("/")) raw else "/$raw"
    val collapsed = absolute.replace(Regex("/+"), "/")
    return if (collapsed.length > 1) collapsed.trimEnd('/') else collapsed
}

/**
 * Relative patterns resolve against [basePath] (the search root), like Conch and SSH. A pattern
 * without '/' matches a basename at any depth.
 */
internal fun globMatch(files: List<String>, pattern: String, basePath: String = "/"): List<String> {
    val cleanPattern = pattern.trim().replace('\\', '/')
    val base = normalizeVirtualPath(basePath).trimEnd('/')
    val adjusted = when {
        cleanPattern.isBlank() -> "$base/**"
        cleanPattern.startsWith("/") -> cleanPattern
        cleanPattern.contains("/") -> "$base/$cleanPattern"
        else -> "$base/**/$cleanPattern"
    }
    return files.filter { file -> PortableGlobMatcher.matches(adjusted, file) }
}
