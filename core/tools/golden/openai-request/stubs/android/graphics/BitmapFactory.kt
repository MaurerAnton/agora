package android.graphics

import java.io.InputStream

/**
 * Harness stub: the golden pipeline compiles the real StreamingJsonRequestBody.kt without the
 * Android framework. The dump itself never decodes pixels (registration reads raw bytes and the
 * trace body comes from segments), so every decode reports "no image" and only the field shapes
 * exist to satisfy compilation.
 */
object BitmapFactory {
    class Options {
        var inJustDecodeBounds: Boolean = false
        var inSampleSize: Int = 1
        var outMimeType: String? = null
        var outWidth: Int = 0
        var outHeight: Int = 0
    }

    fun decodeStream(stream: InputStream, outPadding: IntArray?, opts: Options?): Bitmap? = null

    fun decodeByteArray(data: ByteArray, offset: Int, length: Int, opts: Options?): Bitmap? = null
}

class Bitmap {
    fun recycle(): Unit = Unit
}
