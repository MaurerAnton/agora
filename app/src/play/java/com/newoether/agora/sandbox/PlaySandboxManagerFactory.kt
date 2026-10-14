package com.newoether.agora.sandbox

class PlaySandboxManagerFactory : SandboxManagerFactory {
    override fun create(): SandboxManager = PlaySandboxManager()
    override fun isAvailable(): Boolean = false
}
