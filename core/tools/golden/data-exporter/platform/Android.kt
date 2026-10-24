// Stand-local Android surface for compiling DataExporter.kt verbatim.
//
// DataExporter asks Context for four platform values — the database path used by the snapshot
// reader, the cache directory that hosts the temporary spool, the package version reported in the
// manifest, and the ContentResolver that opens media/destination URIs. The resolver maps the URIs
// a scenario registers to real files; nothing in DataExporter's own traversal lives here.
package android.content

import android.content.pm.PackageManager
import android.net.Uri
import java.io.File
import java.io.InputStream
import java.io.OutputStream

class Context(
    val databaseFile: File,
    val filesDir: File,
    val cacheDir: File,
    val packageName: String,
    val packageManager: PackageManager,
    val contentResolver: ContentResolver,
) {
    val applicationContext: Context
        get() = this

    fun getDatabasePath(name: String): File = databaseFile
}

class ContentResolver {
    /** Scenario-registered URI -> real file. */
    val inputs = mutableMapOf<String, File>()
    val outputs = mutableMapOf<String, File>()

    fun openInputStream(uri: Uri): InputStream? {
        val file = inputs[uri.toString()] ?: fileFor(uri) ?: return null
        return if (file.isFile) file.inputStream() else null
    }

    fun openOutputStream(uri: Uri): OutputStream? {
        val file = outputs[uri.toString()] ?: fileFor(uri) ?: return null
        file.parentFile?.mkdirs()
        return file.outputStream()
    }

    private fun fileFor(uri: Uri): File? =
        if (uri.scheme == "file") File(uri.toString().removePrefix("file://")) else null
}
