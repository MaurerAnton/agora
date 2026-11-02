package com.newoether.agora.util

/**
 * Harness stub: bounds-to-sample math mirrored from the real companion. StreamingJsonRequestBody
 * compiles against these members; the dump never reaches image decoding (BitmapFactory is stubbed
 * to report "no image"), so only the signatures and the sample math have to hold.
 */
internal object ImageArtifactProcessor {
    const val DEFAULT_LONG_EDGE = 2048

    fun sampleSizeForBounds(width: Int, height: Int, maxLongEdge: Int): Int {
        require(width > 0 && height > 0 && maxLongEdge > 0)
        val longest = maxOf(width, height).toLong()
        var sample = 1
        while (longest / (sample.toLong() * 2L) >= maxLongEdge) sample *= 2
        return sample
    }
}
