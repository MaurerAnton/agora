// JVM stand-ins for the two Android/coroutines types MemoryManager.kt depends on.
//
// ONLY the platform surface is stubbed (a file directory and a mutable value holder). The code
// under test — MemoryManager.kt and DescriptionMetadataStore.kt — is compiled VERBATIM from the
// app tree by run_golden.sh; nothing in this file duplicates its logic.
package android.content

import java.io.File

/** Minimal android.content.Context: MemoryManager only ever reads [filesDir]. */
open class Context(open val filesDir: File)
