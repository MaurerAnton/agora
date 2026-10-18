// Stand-only leaf stub: the name/type pair the shell tool builder reads (type is "conch" | "ssh").
package com.newoether.agora.data

data class ShellDeviceConfig(
    val name: String,
    val type: String = "conch",
)
