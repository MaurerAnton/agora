package com.newoether.agora.webui

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkAddress
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import io.mockk.every
import io.mockk.mockk
import io.mockk.mockkStatic
import io.mockk.unmockkStatic
import java.net.InetAddress
import java.net.NetworkInterface
import java.util.Collections
import kotlin.coroutines.EmptyCoroutineContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.flowOf
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class WebUiNetworkAddressesTest {
    private val connectivity = mockk<ConnectivityManager>()
    private val networks = mutableListOf<Network>()
    private val interfaces = mutableListOf<NetworkInterface>()

    init {
        every { connectivity.allNetworks } answers { networks.toTypedArray() }
    }

    @Test
    fun wifiLanRejectsCellularProxyTunAndUnrelatedVirtualAddresses() {
        network("wifi-main", listOf("192.168.31.24"), NetworkCapabilities.TRANSPORT_WIFI)
        network("rmnet_data0", listOf("10.20.0.2", "100.75.1.2"), NetworkCapabilities.TRANSPORT_CELLULAR)
        network("tun0", listOf("172.19.0.1"), NetworkCapabilities.TRANSPORT_VPN, NetworkCapabilities.TRANSPORT_WIFI)
        adapter("docker0", listOf("172.17.0.1"), virtual = true)
        adapter("tun8", listOf("100.90.1.2"), pointToPoint = true)

        assertEquals(listOf("192.168.31.24"), selected())
    }

    @Test
    fun overlayVpnWinsOverItsCellularUnderlyingTransport() {
        network("rmnet_data1", listOf("100.100.1.2"), NetworkCapabilities.TRANSPORT_CELLULAR)
        network("arbitrary-vpn-name", listOf("100.64.0.7", "172.19.0.1"),
            NetworkCapabilities.TRANSPORT_VPN, NetworkCapabilities.TRANSPORT_CELLULAR)
        network("other-vpn", listOf("100.63.255.255", "100.128.0.1"), NetworkCapabilities.TRANSPORT_VPN)

        assertEquals(listOf("100.64.0.7"), selected())
    }

    @Test
    fun lanAddressesAreNotRestrictedToPrivateRangesOrInterfaceNames() {
        network("arbitrary-lan-name", listOf("198.51.100.20"), NetworkCapabilities.TRANSPORT_ETHERNET)
        network("usb-peer", listOf("10.42.0.1"), NetworkCapabilities.TRANSPORT_USB)
        network("bluetooth-peer", listOf("192.168.44.1"), NetworkCapabilities.TRANSPORT_BLUETOOTH)
        network("wifi-no-internet", listOf("192.168.50.2"), NetworkCapabilities.TRANSPORT_WIFI)

        assertEquals(listOf("198.51.100.20", "10.42.0.1", "192.168.44.1", "192.168.50.2"), selected())
    }

    @Test
    fun unregisteredLocalTetheringAdaptersRequireLiveNonTunnelInterfaces() {
        adapter("ap1", listOf("192.168.43.1"))
        adapter("wlan2", listOf("192.168.60.1"))
        adapter("rndis0", listOf("192.168.42.1"))
        adapter("eth3", listOf("198.51.100.4"))
        adapter("wlan4", listOf("192.168.70.1"), up = false)
        adapter("ap2", listOf("192.168.71.1"), virtual = true)
        adapter("usb1", listOf("192.168.72.1"), pointToPoint = true)
        adapter("lo", listOf("127.0.0.1"), loopback = true)
        adapter("veth0", listOf("172.18.0.1"), virtual = true)

        assertEquals(listOf("192.168.43.1", "192.168.60.1", "192.168.42.1", "198.51.100.4"), selected())
    }

    @Test
    fun knownCellularAndProxyLinksCannotUseTheLocalAdapterFallback() {
        network("wlan0", listOf("10.1.1.1"), NetworkCapabilities.TRANSPORT_CELLULAR)
        network("ap0", listOf("172.19.0.1"), NetworkCapabilities.TRANSPORT_VPN)
        val incomplete = network("eth0", listOf("192.168.9.1"), NetworkCapabilities.TRANSPORT_ETHERNET)
        every { connectivity.getNetworkCapabilities(incomplete) } returns null

        assertTrue(selected().isEmpty())
    }

    @Test
    fun rejectsUnusableAndStaleAddressesAndDeduplicatesAssignedHosts() {
        val addresses = listOf("0.0.0.0", "127.0.0.1", "169.254.1.2", "224.0.0.1", "255.255.255.255", "::1", "192.168.1.9")
        network("wifi", addresses, NetworkCapabilities.TRANSPORT_WIFI)
        network("ethernet", listOf("192.168.1.9"), NetworkCapabilities.TRANSPORT_ETHERNET)
        network("down", listOf("192.168.1.10"), NetworkCapabilities.TRANSPORT_WIFI, up = false)
        network("loop", listOf("192.168.1.11"), NetworkCapabilities.TRANSPORT_WIFI, loopback = true)
        network("stale", listOf("192.168.1.12"), NetworkCapabilities.TRANSPORT_WIFI,
            assigned = listOf("192.168.1.13"))

        assertEquals(listOf("192.168.1.9"), selected())
    }

    @Test
    fun networkLookupFailureDoesNotFallBackToUnclassifiedInterfaces() {
        adapter("wlan0", listOf("192.168.31.24"))
        every { connectivity.allNetworks } throws SecurityException("network state unavailable")

        assertTrue(selected().isEmpty())
        val context = mockk<Context>()
        every { context.getSystemService(ConnectivityManager::class.java) } returns null
        assertTrue(webUiNetworkAddresses(context).isEmpty())
    }

    @Test
    fun contextReaderReflectsAddressChangesOnEachRequest() {
        val context = mockk<Context>()
        every { context.getSystemService(ConnectivityManager::class.java) } returns connectivity
        network("wifi", listOf("192.168.31.24"), NetworkCapabilities.TRANSPORT_WIFI)
        mockkStatic(NetworkInterface::class)
        try {
            every { NetworkInterface.getNetworkInterfaces() } answers { Collections.enumeration(interfaces.toList()) }
            assertEquals(listOf("192.168.31.24"), webUiNetworkAddresses(context).map { it.hostAddress })
            networks.clear()
            interfaces.clear()
            network("wifi", listOf("192.168.31.25"), NetworkCapabilities.TRANSPORT_WIFI)
            assertEquals(listOf("192.168.31.25"), webUiNetworkAddresses(context).map { it.hostAddress })
        } finally {
            unmockkStatic(NetworkInterface::class)
        }
    }

    @Test
    fun controllerAccessUrlsUsesFilteredAddressesForHttpAndHttps() {
        val context = mockk<Context>()
        every { context.getSystemService(ConnectivityManager::class.java) } returns connectivity
        val store = mockk<WebUiSettingsStore>()
        every { store.enabled } returns flowOf(false)
        every { store.port } returns flowOf(8686)
        every { store.passwordHash } returns flowOf<String?>(null)
        every { store.https } returns flowOf(true)
        val controller = WebUiController(
            appContext = context,
            store = store,
            scope = CoroutineScope(EmptyCoroutineContext),
            certificates = mockk(),
            syncSession = { _, _, _ -> error("Address listing must not start a sync session") },
            upload = { _, _, _, _, _, _, _, _ -> error("Address listing must not upload") },
            previewAttachment = { _, _, _, _, _, _, _ -> error("Address listing must not preview attachments") },
        )
        network("wifi", listOf("192.168.31.24"), NetworkCapabilities.TRANSPORT_WIFI)
        network("overlay", listOf("100.64.0.7"), NetworkCapabilities.TRANSPORT_VPN)
        network("cellular", listOf("10.20.0.2"), NetworkCapabilities.TRANSPORT_CELLULAR)
        network("proxy", listOf("172.19.0.1"), NetworkCapabilities.TRANSPORT_VPN)
        mockkStatic(NetworkInterface::class)
        try {
            every { NetworkInterface.getNetworkInterfaces() } answers { Collections.enumeration(interfaces.toList()) }
            assertEquals(
                listOf("http://192.168.31.24:8686", "http://100.64.0.7:8686"),
                controller.accessUrls(port = 8686, https = false),
            )
            assertEquals(
                listOf("https://192.168.31.24:9443", "https://100.64.0.7:9443"),
                controller.accessUrls(port = 9443, https = true),
            )
        } finally {
            unmockkStatic(NetworkInterface::class)
        }
    }

    private fun selected(): List<String> = webUiNetworkAddresses(connectivity, interfaces).map { it.hostAddress!! }

    private fun network(
        name: String,
        addresses: List<String>,
        vararg transports: Int,
        assigned: List<String> = addresses,
        up: Boolean = true,
        loopback: Boolean = false,
    ): Network {
        val network = mockk<Network>()
        val capabilities = mockk<NetworkCapabilities>()
        every { capabilities.hasTransport(any()) } answers { firstArg<Int>() in transports }
        val link = mockk<LinkProperties>()
        every { link.interfaceName } returns name
        every { link.linkAddresses } returns addresses.map { host ->
            mockk<LinkAddress>().also { address -> every { address.address } returns InetAddress.getByName(host) }
        }
        every { connectivity.getLinkProperties(network) } returns link
        every { connectivity.getNetworkCapabilities(network) } returns capabilities
        networks += network
        adapter(name, assigned, up = up, loopback = loopback)
        return network
    }

    private fun adapter(
        name: String,
        addresses: List<String>,
        up: Boolean = true,
        loopback: Boolean = false,
        virtual: Boolean = false,
        pointToPoint: Boolean = false,
    ) {
        interfaces += mockk<NetworkInterface>().also { adapter ->
            every { adapter.name } returns name
            every { adapter.isUp } returns up
            every { adapter.isLoopback } returns loopback
            every { adapter.isVirtual } returns virtual
            every { adapter.isPointToPoint } returns pointToPoint
            every { adapter.inetAddresses } answers { Collections.enumeration(addresses.map(InetAddress::getByName)) }
        }
    }
}
