// Platform substitute for util/SecretCrypto.kt: export scenarios never encrypt or decrypt — the
// only observable contract is the "enc:v1:" envelope prefix (SettingsContracts validates it).
// decrypt's pass-through branch for non-prefixed values mirrors the real implementation.
package com.newoether.agora.util

object SecretCrypto {
    private const val PREFIX = "enc:v1:"

    fun isEncrypted(stored: String): Boolean = stored.startsWith(PREFIX)

    fun encrypt(plaintext: String): String = error("SecretCrypto.encrypt is not exercised by export scenarios")

    fun decrypt(stored: String): String =
        if (!stored.startsWith(PREFIX)) stored else error("SecretCrypto.decrypt is not exercised by export scenarios")
}
