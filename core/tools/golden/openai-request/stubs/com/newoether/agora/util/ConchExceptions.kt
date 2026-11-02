package com.newoether.agora.util

import java.io.IOException

/**
 * Harness stub: NetworkRetryPolicy only class-checks these transport failures (and reads
 * NetworkHttpException.statusCode from the real NetworkHttpRetry.kt); the interceptor that
 * raises them never runs under a stand, because HttpClient itself is stubbed.
 */
internal open class ConchChannelException(message: String, cause: Throwable? = null) :
    IOException(message, cause)

internal class ConchAuthenticationException(message: String) : ConchChannelException(message)
