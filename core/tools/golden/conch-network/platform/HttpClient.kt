// Stand-local substitute for com.newoether.agora.api.HttpClient.
//
// The real object owns provider-wide timeouts, proxy selection and diagnostics. ConchNetwork
// only consumes `HttpClient.client` as the base for its own builder, so the stand supplies a
// default OkHttpClient whose four overridden properties and pool are what the traces pin.
package com.newoether.agora.api

import okhttp3.OkHttpClient

internal object HttpClient {
    val client: OkHttpClient = OkHttpClient()
}
