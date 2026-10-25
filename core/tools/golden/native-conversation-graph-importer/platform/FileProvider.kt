// Stand-local androidx.core.content.FileProvider.
//
// NativeConversationMediaRestorer only calls getUriForFile from restoreConversationMedia, which
// this stand's scenarios never run (the driver seeds RestoredMedia directly). The URI shape below
// is the documented stand rule: content://<authority>/<absolute path>.
package androidx.core.content

import android.content.Context
import android.net.Uri
import java.io.File

object FileProvider {
    @JvmStatic
    fun getUriForFile(context: Context, authority: String, file: File): Uri =
        Uri.parse("content://$authority/${file.absolutePath}")
}
