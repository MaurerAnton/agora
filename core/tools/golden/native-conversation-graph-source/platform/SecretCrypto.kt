// Platform substitute for the Android keystore-backed crypto used by settings decoding. The
// policy under test never touches encrypted preferences; the contracts are compiled only for the
// shared settings models, so identity encryption is the faithful stand-in for that surface.
package com.newoether.agora.util

object SecretCrypto {
    fun decrypt(value: String): String = value
    fun encrypt(value: String): String = value
}
