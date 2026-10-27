// Stand-only double of sandbox/SandboxManagerFactory.kt. The real factories are flavor-specific
// (F-Droid process-shared manager, Play no-op); the provider stand scripts one manager and pins
// that ShellToolProvider reads factory availability before listing the Local Sandbox.
package com.newoether.agora.sandbox

internal class ScriptedSandboxManagerFactory(
    val manager: ScriptedSandboxManager = ScriptedSandboxManager(),
) : SandboxManagerFactory {
    var factoryAvailable = true

    override fun create(): SandboxManager = manager

    override fun isAvailable(): Boolean = factoryAvailable
}
