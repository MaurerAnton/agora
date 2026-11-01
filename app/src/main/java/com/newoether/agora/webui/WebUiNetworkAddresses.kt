package com.newoether.agora.webui

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import java.net.Inet4Address
import java.net.NetworkInterface

/** Reads current network state on demand; address presentation does not own a network observer. */
internal fun webUiNetworkAddresses(context: Context): List<Inet4Address> = runCatching {
    val connectivity = context.getSystemService(ConnectivityManager::class.java)
        ?: return@runCatching emptyList()
    webUiNetworkAddresses(connectivity, NetworkInterface.getNetworkInterfaces().toList())
}.getOrDefault(emptyList())

/**
 * LANs come from Android link properties, including non-default LANs while a VPN is active.
 * A VPN transport alone is insufficient: outbound proxy TUNs also register as VPNs. The supported
 * overlay is the Tailscale/Headscale node range, 100.64.0.0/10, on a registered VPN link. Cellular
 * CGNAT addresses never qualify. Unknown VPN ranges remain unadvertised.
 */
internal fun webUiNetworkAddresses(
    connectivity: ConnectivityManager,
    interfaces: List<NetworkInterface>,
): List<Inet4Address> {
    val liveInterfaces = interfaces.mapNotNull { networkInterface ->
        runCatching {
            if (!networkInterface.isUp || networkInterface.isLoopback) return@runCatching null
            networkInterface.name to networkInterface
        }.getOrNull()
    }.toMap()
    val linkedInterfaces = mutableSetOf<String>()
    @Suppress("DEPRECATION") // A one-shot snapshot includes non-default LANs alongside the VPN.
    val networks = runCatching { connectivity.allNetworks.toList() }.getOrElse { return emptyList() }
    val addresses = networks.flatMap { network ->
        runCatching {
            val link = connectivity.getLinkProperties(network)
                ?: return@runCatching emptyList<Inet4Address>()
            link.interfaceName?.let(linkedInterfaces::add)
            val capabilities = connectivity.getNetworkCapabilities(network)
                ?: return@runCatching emptyList<Inet4Address>()
            val vpn = capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN)
            val lan = !vpn && !capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) &&
                listOf(
                    NetworkCapabilities.TRANSPORT_WIFI,
                    NetworkCapabilities.TRANSPORT_ETHERNET,
                    NetworkCapabilities.TRANSPORT_BLUETOOTH,
                    NetworkCapabilities.TRANSPORT_USB,
                ).any(capabilities::hasTransport)
            if (!vpn && !lan) return@runCatching emptyList<Inet4Address>()
            val assigned = liveInterfaces[link.interfaceName]?.inetAddresses?.toList().orEmpty()
            link.linkAddresses.mapNotNull { it.address as? Inet4Address }
                .filter { address ->
                    address in assigned && address.isWebUiHostAddress() &&
                        (!vpn || address.isSupportedOverlayAddress())
                }
        }.getOrDefault(emptyList())
    }
    // Android does not expose downstream tethering links as ConnectivityManager Networks. Retain
    // established local adapter families only when the interface is live, non-virtual and not a
    // point-to-point tunnel. Never use this fallback to override a known cellular/VPN link.
    val localAdapters = liveInterfaces.filterKeys { it !in linkedInterfaces }.values.flatMap { adapter ->
        runCatching {
            if (adapter.isVirtual || adapter.isPointToPoint || !LOCAL_ADAPTER.matches(adapter.name)) {
                emptyList()
            } else {
                adapter.inetAddresses.toList().filterIsInstance<Inet4Address>()
                    .filter { it.isWebUiHostAddress() }
            }
        }.getOrDefault(emptyList())
    }
    return (addresses + localAdapters).distinct()
}

private val LOCAL_ADAPTER = Regex("(?:wlan|swlan|ap|softap|eth|rndis|usb|bnep)\\d+")

private fun Inet4Address.isWebUiHostAddress(): Boolean =
    !isAnyLocalAddress && !isLoopbackAddress && !isLinkLocalAddress && !isMulticastAddress &&
        address.any { (it.toInt() and 0xff) != 255 }

private fun Inet4Address.isSupportedOverlayAddress(): Boolean {
    val bytes = address
    return (bytes[0].toInt() and 0xff) == 100 && (bytes[1].toInt() and 0xff) in 64..127
}
