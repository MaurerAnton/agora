// Stand-local Android surface for compiling NativeConversationGraphImporter.kt,
// NativeConversationMediaRestorer.kt, NativeBackupArchive.kt and friends verbatim.
//
// The importer side needs filesDir/packageName (the media restorer) and the database path (the
// stand ChatDatabase); NativeBackupArchive needs ContentResolver.openFileDescriptor with the
// stand's ParcelFileDescriptor substitute. The URI stream surface (openInputStream/openOutputStream
// with scenario-registered files) is the media/destination boundary the export side shares.
package android.content

import android.content.pm.PackageManager
import android.net.Uri
import android.os.ParcelFileDescriptor
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

class ContentResolver(
    private val descriptorFor: (Uri) -> ParcelFileDescriptor? = { null },
) {
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

    /** The archive's channel path: read-only descriptors over a stand-local file. */
    fun openFileDescriptor(uri: Uri, mode: String): ParcelFileDescriptor? {
        require(mode == "r") { "unsupported open mode: $mode" }
        return descriptorFor(uri)
    }

    private fun fileFor(uri: Uri): File? =
        if (uri.scheme == "file") File(uri.toString().removePrefix("file://")) else null
}
