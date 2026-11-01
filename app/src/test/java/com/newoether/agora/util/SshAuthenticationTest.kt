package com.newoether.agora.util

import com.jcraft.jsch.JSch
import com.jcraft.jsch.KeyPair
import com.newoether.agora.data.SSH_AUTH_PRIVATE_KEY
import com.newoether.agora.data.ShellDeviceConfig
import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.InputStream

class SshAuthenticationTest {
    @Test
    fun plainAndEncryptedKeysParseAndPassphrasesAreCheckedWithoutLeakingInput() {
        val plain = privateKey()
        assertFalse(validateSshPrivateKey(plain))
        val encrypted = privateKey("correct passphrase")
        assertTrue(validateSshPrivateKey(encrypted))
        assertTrue(validateSshPrivateKey(encrypted, "correct passphrase"))
        val failure = runCatching { validateSshPrivateKey(encrypted, "wrong passphrase") }.exceptionOrNull()
        assertEquals(SshPrivateKeyException.Reason.PASSPHRASE, (failure as SshPrivateKeyException).reason)
        assertFalse(failure.message.orEmpty().contains("wrong passphrase"))
        assertFalse(failure.message.orEmpty().contains(encrypted))
        assertNull(failure.cause)
    }

    @Test
    fun invalidOrPublicOnlyFilesAreRejectedAndBoundedImportStopsAtOneExtraByte() {
        listOf("", "not a private key", "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOpublic-only").forEach {
            val failure = runCatching { readSshPrivateKey(it.byteInputStream()) }.exceptionOrNull()
            assertEquals(SshPrivateKeyException.Reason.INVALID, (failure as SshPrivateKeyException).reason)
        }
        var consumed = 0
        val endless = object : InputStream() {
            override fun read(): Int { consumed++; return 'a'.code }
            override fun read(bytes: ByteArray, offset: Int, length: Int): Int {
                bytes.fill('a'.code.toByte(), offset, offset + length)
                consumed += length
                return length
            }
        }
        val failure = runCatching { readSshPrivateKey(endless) }.exceptionOrNull() as SshPrivateKeyException
        assertEquals(SshPrivateKeyException.Reason.TOO_LARGE, failure.reason)
        assertEquals(SSH_PRIVATE_KEY_MAX_BYTES + 1, consumed)
        val plain = privateKey()
        assertEquals(plain to false, readSshPrivateKey(plain.byteInputStream()))
    }

    @Test
    fun malformedUtf8AndZeroLengthReadsCannotBypassTheBound() {
        assertTrue(runCatching { readSshPrivateKey(byteArrayOf(0xc3.toByte(), 0x28).inputStream()) }
            .exceptionOrNull() is SshPrivateKeyException)
        var consumed = 0
        val input = object : InputStream() {
            override fun read(bytes: ByteArray, offset: Int, length: Int): Int = 0
            override fun read(): Int { consumed++; return 1 }
        }
        val failure = runCatching { readSshPrivateKey(input) }.exceptionOrNull() as SshPrivateKeyException
        assertEquals(SshPrivateKeyException.Reason.TOO_LARGE, failure.reason)
        assertEquals(SSH_PRIVATE_KEY_MAX_BYTES + 1, consumed)
    }

    @Test
    fun passwordDefaultIgnoresStaleKeyAndPrivateModeNeverFallsBackToPassword() {
        val jsch = JSch()
        val session = jsch.getSession("user", "example.test", 22)
        val password = ShellDeviceConfig(name = "SSH", sshPassword = "password", sshPrivateKey = "invalid stale key")
        configureSshAuthentication(jsch, session, password)
        assertEquals("password", session.getConfig("PreferredAuthentications"))
        assertTrue(jsch.identityNames.isEmpty())
        val keyed = password.copy(sshAuthMode = SSH_AUTH_PRIVATE_KEY, sshPrivateKey = privateKey("unlock"), sshKeyPassphrase = "unlock")
        try {
            configureSshAuthentication(jsch, session, keyed)
            assertEquals("publickey", session.getConfig("PreferredAuthentications"))
            val identity = jsch.identityRepository.identities.single()
            assertFalse(identity.isEncrypted)
            assertNotNull(identity.getSignature("authentication challenge".toByteArray(), "rsa-sha2-256"))
        } finally {
            jsch.removeAllIdentity()
        }
        assertTrue(jsch.identityNames.isEmpty())
        val failure = runCatching { configureSshAuthentication(jsch, session, keyed.copy(sshKeyPassphrase = "wrong")) }
            .exceptionOrNull()
        assertTrue(failure is SshPrivateKeyException)
        assertTrue(jsch.identityNames.isEmpty())
    }

    private fun privateKey(passphrase: String? = null): String {
        val key = KeyPair.genKeyPair(JSch(), KeyPair.RSA, 2048)
        return try {
            val output = ByteArrayOutputStream()
            key.writePrivateKey(output, passphrase?.toByteArray())
            output.toString(Charsets.UTF_8.name())
        } finally {
            key.dispose()
        }
    }
}
