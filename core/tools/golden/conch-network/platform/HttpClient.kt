// Stand-local substitute for com.newoether.agora.api.HttpClient.
//
// The real object owns provider-wide timeouts, proxy selection and diagnostics. ConchNetwork
// only consumes `HttpClient.client` as the base for its own builder, so the stand supplies a
// default OkHttpClient whose four overridden properties and pool are what the traces pin.
package com.newoether.agora.api

import okhttp3.OkHttpClient
import com.newoether.agora.viewmodel.StreamScope

internal object HttpClient {
    val client: OkHttpClient = OkHttpClient()

    // The stand never cancels an in-flight request, which is the read condition
    // executeWithNetworkRetry threads through its default parameter.
    fun isRequestCancelled(): Boolean = false

    // No coroutine stream scope exists outside a real generation; a null scope skips registration.
    fun boundStreamScope(): StreamScope? = null
}
