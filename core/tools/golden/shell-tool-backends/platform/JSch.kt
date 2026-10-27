package com.jcraft.jsch

import java.io.ByteArrayInputStream
import java.io.InputStream
import java.io.OutputStream

/**
 * Stand-local scripted subset of the JSch API used by util/SshClient.kt, which compiles
 * VERBATIM against it. Every session/channel/SFTP action is recorded so the trace can pin
 * SshClient's own logic: StrictHostKeyChecking decisions, the TOFU host-key check (driven by
 * SshClient's real HostKeyRepository implementation, so capturedHostKey and the
 * OK/CHANGED/NOT_INCLUDED decisions are the genuine article), the assembled exec command
 * (workdir `cd` + escapeBash), stdout/stderr collection, the isClosed/exitStatus loop and the
 * stat/mkdir/get/put/pwd/ls SFTP sequence.
 */

// ---- Script state -------------------------------------------------------------------------------

data class StatScript(val isDir: Boolean, val size: Long)

sealed class JschEvent {
    data class SetConfig(val key: String, val value: String) : JschEvent()
    data class SetPassword(val password: String) : JschEvent()
    data class HostKeyCheck(val host: String, val decision: Int, val presentedB64: String) : JschEvent()
    data class Connect(val channel: String, val timeoutMs: Int) : JschEvent()
    data class OpenChannel(val kind: String) : JschEvent()
    data class SetCommand(val command: String) : JschEvent()
    data class Disconnect(val channel: String) : JschEvent()
    data class Stat(val path: String, val found: Boolean) : JschEvent()
    data class Mkdir(val path: String) : JschEvent()
    data class Get(val path: String) : JschEvent()
    data class Put(val path: String, val bytes: Int) : JschEvent()
    data class Ls(val dir: String) : JschEvent()
    data class Pwd(val path: String) : JschEvent()
}

object JschScript {
    var presentedKey: ByteArray = byteArrayOf()
    /** Thrown from Session.connect when the repository rejects (strict mode) — message scripted. */
    var strictRejectionMessage: String = "host key rejected"
    /** Thrown from Session.connect before the host-key exchange (generic connection failure). */
    var connectFailure: String? = null
    /** Thrown from Session.openChannel ("exec"|"sftp"). */
    var openChannelFailure: String? = null
    var neverClose = false
    var exitStatus = 0
    var stdout: ByteArray = ByteArray(0)
    var stderr: ByteArray = ByteArray(0)
    var stats: Map<String, StatScript> = emptyMap()
    var mkdirFailures: Set<String> = emptySet()
    var putFailures: Map<String, String> = emptyMap()
    var fileBytes: Map<String, ByteArray> = emptyMap()
    /** Second and later reads of a path return these bytes when present (edit guard). */
    var fileBytesSecond: Map<String, ByteArray> = emptyMap()
    private val getCounts = mutableMapOf<String, Int>()
    var lsEntries: Map<String, List<Pair<String, Boolean>>> = emptyMap()
    var pwdPath: String = "/home/user"

    val events = mutableListOf<JschEvent>()

    /** Returns how many reads of this path happened before this one. */
    fun bumpGet(path: String): Int {
        val seen = getCounts[path] ?: 0
        getCounts[path] = seen + 1
        return seen
    }

    fun reset() {
        presentedKey = byteArrayOf()
        strictRejectionMessage = "host key rejected"
        connectFailure = null
        openChannelFailure = null
        neverClose = false
        exitStatus = 0
        stdout = ByteArray(0)
        stderr = ByteArray(0)
        stats = emptyMap()
        mkdirFailures = emptySet()
        putFailures = emptyMap()
        fileBytes = emptyMap()
        fileBytesSecond = emptyMap()
        getCounts.clear()
        lsEntries = emptyMap()
        pwdPath = "/home/user"
        events.clear()
    }
}

// ---- API surface ---------------------------------------------------------------------------------

class JSch {
    fun getSession(user: String, host: String, port: Int): Session =
        Session(user, host, port)
}

/** Type-only in SshClient (Array<HostKey> and the add parameter). */
class HostKey

/** Type-only (the HostKeyRepository.add parameter). */
interface UserInfo

interface HostKeyRepository {
    fun check(host: String?, key: ByteArray): Int
    fun add(hostkey: HostKey?, ui: UserInfo?)
    fun remove(host: String?, type: String?)
    fun remove(host: String?, type: String?, key: ByteArray?)
    fun getKnownHostsRepositoryID(): String
    fun getHostKey(): Array<HostKey>
    fun getHostKey(host: String?, type: String?): Array<HostKey>

    companion object {
        const val OK = 0
        const val CHANGED = 1
        const val NOT_INCLUDED = 2
    }
}

open class Channel

class Session internal constructor(val user: String, val host: String, val port: Int) {
    var hostKeyRepository: HostKeyRepository? = null
    private val config = mutableMapOf<String, String>()
    private var password: String? = null
    var isConnected = false
        private set

    fun setPassword(value: String) {
        password = value
        JschScript.events.add(JschEvent.SetPassword(value))
    }

    fun setConfig(key: String, value: String) {
        config[key] = value
        JschScript.events.add(JschEvent.SetConfig(key, value))
    }

    fun connect(timeoutMs: Int) {
        JschScript.events.add(JschEvent.Connect("session", timeoutMs))
        JschScript.connectFailure?.let { throw Exception(it) }
        // JSch consults the repository during the handshake, then applies StrictHostKeyChecking.
        val decision = hostKeyRepository?.check(host, JschScript.presentedKey)
            ?: HostKeyRepository.NOT_INCLUDED
        if (hostKeyRepository != null) {
            // What SshClient's repository stores as capturedHostKey: Base64 with NO_WRAP ==
            // java.util.Base64.getEncoder() (standard alphabet, padded, unwrapped).
            JschScript.events.add(
                JschEvent.HostKeyCheck(
                    host, decision,
                    java.util.Base64.getEncoder().encodeToString(JschScript.presentedKey),
                ),
            )
        }
        val strict = config["StrictHostKeyChecking"] == "yes"
        if (strict && decision != HostKeyRepository.OK) {
            throw Exception(JschScript.strictRejectionMessage)
        }
        isConnected = true
    }

    fun disconnect() {
        isConnected = false
        JschScript.events.add(JschEvent.Disconnect("session"))
    }

    fun openChannel(kind: String): Channel {
        JschScript.events.add(JschEvent.OpenChannel(kind))
        JschScript.openChannelFailure?.let { throw Exception(it) }
        return when (kind) {
            "exec" -> ChannelExec()
            "sftp" -> ChannelSftp()
            else -> error("unsupported channel $kind")
        }
    }
}

class ChannelExec : Channel() {
    private var command: String? = null
    private var stdoutStream: OutputStream? = null
    private var stderrStream: OutputStream? = null
    var isClosed = false
        private set
    var exitStatus = 0
        private set

    fun setCommand(value: String) {
        command = value
        JschScript.events.add(JschEvent.SetCommand(value))
    }

    fun setOutputStream(stream: OutputStream) {
        stdoutStream = stream
    }

    fun setErrStream(stream: OutputStream) {
        stderrStream = stream
    }

    fun connect(timeoutMs: Int) {
        JschScript.events.add(JschEvent.Connect("exec", timeoutMs))
        JschScript.connectFailure?.let { throw Exception(it) }
        stdoutStream?.write(JschScript.stdout)
        stderrStream?.write(JschScript.stderr)
        exitStatus = JschScript.exitStatus
        if (!JschScript.neverClose) isClosed = true
    }

    fun disconnect() {
        JschScript.events.add(JschEvent.Disconnect("exec"))
    }
}

class ChannelSftp : Channel() {
    class LsEntry(val filename: String, val attrs: SftpATTRS)

    companion object {
        const val OVERWRITE = 2
    }

    fun connect(timeoutMs: Int) {
        JschScript.events.add(JschEvent.Connect("sftp", timeoutMs))
        JschScript.connectFailure?.let { throw Exception(it) }
    }

    fun disconnect() {
        JschScript.events.add(JschEvent.Disconnect("sftp"))
    }

    fun stat(path: String): SftpATTRS {
        val script = JschScript.stats[path]
        JschScript.events.add(JschEvent.Stat(path, script != null))
        if (script == null) throw Exception("No such file: $path")
        return SftpATTRS(script.isDir, script.size)
    }

    fun mkdir(path: String) {
        JschScript.events.add(JschEvent.Mkdir(path))
        if (path in JschScript.mkdirFailures) throw Exception("Permission denied: $path")
    }

    fun get(path: String): InputStream {
        JschScript.events.add(JschEvent.Get(path))
        val seen = JschScript.bumpGet(path)
        val bytes = if (seen == 0) {
            JschScript.fileBytes[path]
        } else {
            JschScript.fileBytesSecond[path] ?: JschScript.fileBytes[path]
        } ?: throw Exception("No such file: $path")
        return ByteArrayInputStream(bytes)
    }

    fun put(input: InputStream, path: String, mode: Int) {
        JschScript.events.add(JschEvent.Put(path, input.readBytes().size))
        JschScript.putFailures[path]?.let { throw Exception(it) }
    }

    fun pwd(): String {
        JschScript.events.add(JschEvent.Pwd(JschScript.pwdPath))
        return JschScript.pwdPath
    }

    fun ls(dir: String): List<LsEntry> {
        JschScript.events.add(JschEvent.Ls(dir))
        val entries = JschScript.lsEntries[dir] ?: throw Exception("No such directory: $dir")
        return entries.map { (name, isDir) -> LsEntry(name, SftpATTRS(isDir, 0)) }
    }
}

class SftpATTRS(val isDir: Boolean, val size: Long)
