package com.newoether.agora.viewmodel

import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.util.SshClient
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal class HostKeyCaptureSession(
    val probe: suspend () -> Unit,
    val capturedHostKey: () -> String?,
    val close: () -> Unit,
)

/** Captures a server key for explicit review. Authentication uses the current editor draft. */
internal class SshHostKeyVerifier(
    private val createSession: (ShellDeviceConfig) -> HostKeyCaptureSession = ::createHostKeyCaptureSession,
    private val fingerprint: (String) -> String = SshClient::fingerprintSha256,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
) {
    suspend fun verify(device: ShellDeviceConfig): Result<Pair<String, String>> = withContext(ioDispatcher) {
        if (device.sshHost.isBlank()) return@withContext Result.failure(Exception("Host is empty"))
        val session = createSession(device.copy(sshUser = device.sshUser.ifBlank { "root" }))
        try {
            try {
                session.probe()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                // Authentication may fail after the handshake; the presented host key is still valid.
            }
        } finally {
            session.close()
        }
        val key = session.capturedHostKey()
        if (key.isNullOrBlank()) {
            Result.failure(Exception("Could not reach host or no host key presented"))
        } else {
            Result.success(key to fingerprint(key))
        }
    }
}

private fun createHostKeyCaptureSession(device: ShellDeviceConfig): HostKeyCaptureSession {
    val client = SshClient(
        device = device,
        pinnedHostKey = "",
        allowUnknownHostKey = true,
    )
    return HostKeyCaptureSession(
        probe = { client.executeCommand("true") },
        capturedHostKey = { client.capturedHostKey },
        close = client::close,
    )
}
