package com.newoether.agora.api.util.tokens

/**
 * Fork-stand compat copy: upstream commit 3e7ebc90 dropped the encoded-length rule from the app
 * core ("Encoded length is retained for compatibility, never priced"), but the stand keeps the
 * byte_* regressions pinned to the historical rule. Values must stay identical to the core's
 * byte_proportional_image_tokens.
 */
object ByteProportionalImageCost {
    const val BYTES_PER_TOKEN = 768L
    const val MIN_TOKENS = 256L
    const val MAX_TOKENS = 1_536L

    /** Cost of an image whose durable metadata records no usable byte size. */
    const val UNKNOWN_TOKENS = 1_024L

    fun tokens(image: ImageDescriptor): Long = image.byteSize
        ?.takeIf { it > 0L }
        ?.let { bytes -> (bytes / BYTES_PER_TOKEN).coerceIn(MIN_TOKENS, MAX_TOKENS) }
        ?: UNKNOWN_TOKENS
}
