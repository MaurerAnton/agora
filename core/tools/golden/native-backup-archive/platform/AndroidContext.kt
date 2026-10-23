// Stand-local android.content surface for NativeBackupArchive.open(context, uri).
//
// The production path resolves the Uri through the content resolver to a seekable file
// descriptor; the stand resolves it to a local File so the same channel code runs.
package android.content

import android.net.Uri
import android.os.ParcelFileDescriptor

class ContentResolver(private val descriptorFor: (Uri) -> ParcelFileDescriptor?) {
    fun openFileDescriptor(uri: Uri, mode: String): ParcelFileDescriptor? {
        require(mode == "r") { "unsupported open mode: $mode" }
        return descriptorFor(uri)
    }
}

class Context(val contentResolver: ContentResolver)
