// Stand-local android.os.ParcelFileDescriptor: enough of the surface for the archive's
// `AutoCloseInputStream(descriptor).channel` — a real seekable FileInputStream channel.
package android.os

import java.io.File
import java.io.FileInputStream

class ParcelFileDescriptor(val file: File) {
    class AutoCloseInputStream(descriptor: ParcelFileDescriptor) : FileInputStream(descriptor.file)
}
