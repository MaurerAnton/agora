package com.newoether.agora.util

import com.jcraft.jsch.JSch
import com.jcraft.jsch.KeyPair
import com.jcraft.jsch.Session
import com.newoether.agora.data.SSH_AUTH_PASSWORD
import com.newoether.agora.data.SSH_AUTH_PRIVATE_KEY
import com.newoether.agora.data.ShellDeviceConfig
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

internal const val SSH_PRIVATE_KEY_MAX_BYTES = 256 * 1024

internal class SshPrivateKeyException(val reason: Reason) : IllegalArgumentException(reason.message) {
    enum class Reason(val message: String) {
        INVALID("Invalid or unsupported SSH private key"),
        TOO_LARGE("SSH private key exceeds 256 KiB"),
        PASSPHRASE("Incorrect SSH private key passphrase"),
    }
}

/** Bounded in-memory import. The caller owns the stream; no URI or private key file is retained. */
internal fun readSshPrivateKey(input: InputStream): Pair<String, Boolean> {
    val output = ByteArrayOutputStream()
    val buffer = ByteArray(8192)
    try {
        while (true) {
            val count = input.read(buffer, 0, minOf(buffer.size, SSH_PRIVATE_KEY_MAX_BYTES + 1 - output.size()))
            if (count < 0) break
            if (count == 0) {
                val next = input.read()
                if (next < 0) break
                output.write(next)
            } else {
                output.write(buffer, 0, count)
            }
            if (output.size() > SSH_PRIVATE_KEY_MAX_BYTES) {
                throw SshPrivateKeyException(SshPrivateKeyException.Reason.TOO_LARGE)
            }
        }
        val bytes = output.toByteArray()
        val text = try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes)).toString()
        } catch (_: Exception) {
            throw SshPrivateKeyException(SshPrivateKeyException.Reason.INVALID)
        } finally {
            bytes.fill(0)
        }
        return text to validateSshPrivateKey(text)
    } finally {
        buffer.fill(0)
        output.reset()
    }
}

/** Returns whether a parsed key is encrypted, optionally checking its passphrase before Save. */
internal fun validateSshPrivateKey(text: String, passphrase: String? = null): Boolean {
    if ('\u0000' in text || !Regex("-----BEGIN (?:OPENSSH |RSA |DSA |EC |ENCRYPTED )?PRIVATE KEY-----")
            .containsMatchIn(text)) {
        throw SshPrivateKeyException(SshPrivateKeyException.Reason.INVALID)
    }
    val bytes = text.toByteArray(Charsets.UTF_8)
    if (bytes.size > SSH_PRIVATE_KEY_MAX_BYTES) {
        bytes.fill(0)
        throw SshPrivateKeyException(SshPrivateKeyException.Reason.TOO_LARGE)
    }
    val key = try {
        KeyPair.load(JSch(), bytes, null)
            ?: throw SshPrivateKeyException(SshPrivateKeyException.Reason.INVALID)
    } catch (_: Exception) {
        throw SshPrivateKeyException(SshPrivateKeyException.Reason.INVALID)
    } finally {
        bytes.fill(0)
    }
    try {
        val encrypted = key.isEncrypted
        if (encrypted && passphrase != null) {
            val passphraseBytes = passphrase.toByteArray(Charsets.UTF_8)
            try {
                if (!key.decrypt(passphraseBytes)) {
                    throw SshPrivateKeyException(SshPrivateKeyException.Reason.PASSPHRASE)
                }
            } catch (_: Exception) {
                throw SshPrivateKeyException(SshPrivateKeyException.Reason.PASSPHRASE)
            } finally {
                passphraseBytes.fill(0)
            }
        }
        return encrypted
    } finally {
        key.dispose()
    }
}

/** Both ordinary tools and host-key verification call this exact authentication selection. */
internal fun configureSshAuthentication(jsch: JSch, session: Session, device: ShellDeviceConfig) {
    jsch.removeAllIdentity()
    when (device.sshAuthMode) {
        SSH_AUTH_PASSWORD -> {
            session.setPassword(device.sshPassword)
            session.setConfig("PreferredAuthentications", "password")
        }
        SSH_AUTH_PRIVATE_KEY -> {
            session.setPassword("")
            validateSshPrivateKey(device.sshPrivateKey, device.sshKeyPassphrase)
            val keyBytes = device.sshPrivateKey.toByteArray(Charsets.UTF_8)
            val passphraseBytes = device.sshKeyPassphrase.toByteArray(Charsets.UTF_8)
            try {
                jsch.addIdentity("agora-ssh", keyBytes, null, passphraseBytes)
                session.setConfig("PreferredAuthentications", "publickey")
            } catch (_: Exception) {
                jsch.removeAllIdentity()
                throw SshPrivateKeyException(SshPrivateKeyException.Reason.INVALID)
            } finally {
                keyBytes.fill(0)
                passphraseBytes.fill(0)
            }
        }
        else -> throw IllegalArgumentException("Unsupported SSH authentication method")
    }
}
