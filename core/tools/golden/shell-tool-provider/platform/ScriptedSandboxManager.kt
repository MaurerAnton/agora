package com.newoether.agora.sandbox

import com.newoether.agora.util.ShellFileEditResult
import com.newoether.agora.util.ShellFileReadResult
import java.io.File
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.flowOf

/**
 * Stand-local scripted implementation of the SandboxManager interface (which compiles VERBATIM
 * from the app tree). SandboxBackend exercises isAvailable/executeCommand/fileRead/fileWrite/
 * fileGlob/fileGrep/fileEdit; every call is recorded so the trace pins the backend's wrapping,
 * and each result/failure is scripted per scenario. Members the shell backends never touch
 * (rootfs install, apk lifecycle) keep inert defaults — they are interface surface, not pinned
 * behavior of this module.
 */
internal class ScriptedSandboxManager : SandboxManager {
    override val terminalOutput: StateFlow<String> = MutableStateFlow("")
    override val packageList: StateFlow<List<SandboxManager.PackageInfo>> =
        MutableStateFlow(emptyList())
    override val snackbarMessage: Flow<String> = flowOf()
    override val isBusy: StateFlow<Boolean> = MutableStateFlow(false)
    override val isInstallingRootfs: StateFlow<Boolean> = MutableStateFlow(false)
    override val downloadProgress: StateFlow<Float?> = MutableStateFlow(null)
    override val lastError: String? = null
    override var pendingPkgName: String = ""

    var available = true
    var executeResult: SandboxManager.SandboxResult? = null
    var executeFailure: String? = null
    var fileReadResult: ShellFileReadResult? = null
    var fileReadFailure: String? = null
    var fileWriteError: String? = null
    var fileWriteFailure: String? = null
    var fileEditResult: ShellFileEditResult? = null
    var fileEditFailure: String? = null
    var fileGlobResult: Pair<List<String>, Boolean>? = null
    var fileGlobFailure: String? = null
    var fileGrepResult: Pair<List<SandboxManager.GrepMatch>, Boolean>? = null
    var fileGrepFailure: String? = null

    val calls = mutableListOf<String>()

    fun clearScript() {
        available = true
        executeResult = null
        executeFailure = null
        fileReadResult = null
        fileReadFailure = null
        fileWriteError = null
        fileWriteFailure = null
        fileEditResult = null
        fileEditFailure = null
        fileGlobResult = null
        fileGlobFailure = null
        fileGrepResult = null
        fileGrepFailure = null
        calls.clear()
    }

    override suspend fun refreshPackageList() {
        calls.add("refreshPackageList")
    }

    override fun installRootfs() {
        calls.add("installRootfs")
    }

    override fun installPackage(name: String) {
        calls.add("installPackage:$name")
    }

    override fun removePackage(name: String) {
        calls.add("removePackage:$name")
    }

    override fun upgradePackages() {
        calls.add("upgradePackages")
    }

    override fun isAvailableSync(): Boolean = available

    override suspend fun isAvailable(): Boolean {
        calls.add("isAvailable")
        return available
    }

    override suspend fun install(): Boolean {
        calls.add("install")
        return false
    }

    override suspend fun executeCommand(
        command: String,
        workdir: String,
        timeoutMs: Int,
    ): SandboxManager.SandboxResult {
        calls.add("executeCommand")
        executeFailure?.let { throw Exception(it) }
        return executeResult ?: SandboxManager.SandboxResult("", "", 0)
    }

    override suspend fun fileRead(
        path: String,
        offset: Long,
        limit: Long,
    ): ShellFileReadResult {
        calls.add("fileRead:$path")
        fileReadFailure?.let { throw Exception(it) }
        return fileReadResult ?: ShellFileReadResult(
            content = "", lines = 0, totalLines = 0, totalBytes = 0, returnedBytes = 0,
            offset = offset.coerceAtLeast(0), limit = limit, truncated = false,
        )
    }

    override suspend fun fileWrite(path: String, content: String): String? {
        calls.add("fileWrite:$path")
        fileWriteFailure?.let { throw Exception(it) }
        return fileWriteError
    }

    override suspend fun fileGlob(
        pattern: String,
        basePath: String,
        depth: Int?,
    ): Pair<List<String>, Boolean> {
        calls.add("fileGlob:$pattern")
        fileGlobFailure?.let { throw Exception(it) }
        return fileGlobResult ?: (emptyList<String>() to false)
    }

    override suspend fun fileGrep(
        pattern: String,
        basePath: String,
        fileGlob: String,
    ): Result<Pair<List<SandboxManager.GrepMatch>, Boolean>> {
        calls.add("fileGrep:$pattern")
        fileGrepFailure?.let { return Result.failure(Exception(it)) }
        return Result.success(
            fileGrepResult ?: (emptyList<SandboxManager.GrepMatch>() to false),
        )
    }

    override suspend fun fileEdit(
        path: String,
        oldString: String,
        newString: String,
        replaceAll: Boolean,
    ): ShellFileEditResult {
        calls.add("fileEdit:$path")
        fileEditFailure?.let { throw Exception(it) }
        return fileEditResult ?: ShellFileEditResult(replacements = 0)
    }

    override suspend fun apkInstall(
        packageName: String,
        onProgress: (String) -> Unit,
    ): Boolean {
        calls.add("apkInstall:$packageName")
        return false
    }

    override suspend fun apkUpgrade(onProgress: (String) -> Unit): Int {
        calls.add("apkUpgrade")
        return 0
    }

    override suspend fun apkList(): List<SandboxManager.PackageInfo> {
        calls.add("apkList")
        return emptyList()
    }

    override suspend fun apkDelete(packageName: String): Boolean {
        calls.add("apkDelete:$packageName")
        return false
    }

    override suspend fun getDiskUsageMB(): Long {
        calls.add("getDiskUsageMB")
        return 0
    }

    override suspend fun reset(): Boolean {
        calls.add("reset")
        return false
    }

    fun scriptReady(): Boolean = true

    override fun getSandboxHomeDir(): File? = null
}
