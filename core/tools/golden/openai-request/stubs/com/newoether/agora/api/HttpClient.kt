package com.newoether.agora.api

/**
 * Harness stub: the golden dump only builds a request body, so no request is ever in flight and
 * cancellation is never observed. The real HttpClient (HTTP/2, streaming, cancellation scopes)
 * stays out of the harness source closure.
 */
object HttpClient {
    fun isRequestCancelled(): Boolean = false
}
