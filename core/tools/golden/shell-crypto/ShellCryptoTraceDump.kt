// Golden trace driver for util/ShellCrypto.kt.
//
// ShellCrypto.kt compiles VERBATIM from the app tree. Only the android.util.Base64
// surface is substituted (platform/). The stand exercises every public function
// with deterministic inputs and freezes the output trace.
package shell.crypto.golden

import android.util.Base64
import com.newoether.agora.util.ShellCrypto
import java.nio.charset.StandardCharsets
import java.security.KeyFactory
import java.security.KeyPair
import java.security.spec.PKCS8EncodedKeySpec
import java.security.spec.X509EncodedKeySpec
import java.util.Base64 as JvmBase64

// Hardcoded X25519 SPKI prefix (12 bytes): SEQUENCE(0x30, 0x2a=42) + AlgorithmIdentifier + BIT STRING header
// 30 2a 30 05 06 03 2b 65 6e 03 21 00
private val X25519_SPKI_PREFIX = byteArrayOf(
    0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x6e, 0x03, 0x21, 0x00
)

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
        val outputDir = java.io.File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = java.io.File(path)
            java.io.File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(java.io.File(args[0])))
}

private fun jsonObj(vararg pairs: Pair<String, Any?>): String {
    val sb = StringBuilder("{")
    var first = true
    for ((k, v) in pairs) {
        if (!first) sb.append(",")
        first = false
        sb.append("\"$k\":")
        when (v) {
            null -> sb.append("null")
            is String -> sb.append("\"").append(esc(v)).append("\"")
            is Boolean -> sb.append(v.toString())
            is Number -> sb.append(v.toString())
            else -> sb.append("\"").append(esc(v.toString())).append("\"")
        }
    }
    sb.append("}")
    return sb.toString()
}

private fun esc(s: String): String = s.replace("\\\\".toRegex(), "\\\\").replace("\"".toRegex(), "\\\"").replace("\n".toRegex(), "\\n").replace("\r".toRegex(), "\\r").replace("\t".toRegex(), "\\t")

// Simple JSON parsing using a minimal parser
private fun parseJson(text: String): Map<String, Any?> {
    val parser = JsonParser(text)
    return parser.parseObject()
}

private class JsonParser(private val text: String) {
    private var pos = 0

    private fun peek_(): Char = if (pos < text.length) text[pos] else '\u0000'
    private fun consume_(): Char = text[pos++]
    private fun skipWs_() { while (pos < text.length && text[pos].isWhitespace()) pos++ }

    fun parseObject(): Map<String, Any?> {
        skipWs_()
        if (consume_() != '{') error("Expected {")
        val map = mutableMapOf<String, Any?>()
        while (true) {
            skipWs_()
            if (peek_() == '}') { consume_(); break }
            val key = parseString_()
            skipWs_()
            if (consume_() != ':') error("Expected :")
            val value = parseValue_()
            map[key] = value
            skipWs_()
            val c = peek_()
            if (c == '}') { consume_(); break }
            if (c == ',') { consume_(); } else error("Expected , or }")
        }
        return map
    }

    private fun parseValue_(): Any? {
        skipWs_()
        val c = peek_()
        return when (c) {
            '"' -> parseString_()
            '[' -> { consume_(); parseArray_() }
            '{' -> parseObject_()
            't' -> { consume_(); consume_(); consume_(); consume_(); true }
            'f' -> { consume_(); consume_(); consume_(); consume_(); consume_(); false }
            'n' -> { consume_(); consume_(); consume_(); consume_(); consume_(); null }
            else -> parseNumber_()
        }
    }

    private fun parseArray_(): List<Any?> {
        val list = mutableListOf<Any?>()
        while (true) {
            skipWs_()
            if (peek_() == ']') { consume_(); break }
            list.add(parseValue_())
            skipWs_()
            val c = peek_()
            if (c == ']') { consume_(); break }
            if (c == ',') consume_() else error("Expected , or ]")
        }
        return list
    }

    private fun parseObject_(): Map<String, Any?> {
        skipWs_()
        if (consume_() != '{') error("Expected {")
        val map = mutableMapOf<String, Any?>()
        while (true) {
            skipWs_()
            if (peek_() == '}') { consume_(); break }
            val key = parseString_()
            skipWs_()
            if (consume_() != ':') error("Expected :")
            val value = parseValue_()
            map[key] = value
            skipWs_()
            val c = peek_()
            if (c == '}') { consume_(); break }
            if (c == ',') { consume_(); } else error("Expected , or }")
        }
        return map
    }

    private fun parseString_(): String {
        consume_() // consume opening "
        val sb = StringBuilder()
        while (true) {
            val c = consume_()
            if (c == '"') break
            if (c == '\\') {
                val next = consume_()
                sb.append(when (next) {
                    '"' -> '"'
                    '\\' -> '\\'
                    '/' -> '/'
                    'b' -> '\b'
                    'f' -> '\u000C'
                    'n' -> '\n'
                    'r' -> '\r'
                    't' -> '\t'
                    'u' -> {
                        val hex = text.substring(pos, pos + 4)
                        pos += 4
                        hex.toInt(16).toChar()
                    }
                    else -> next
                })
            } else {
                sb.append(c)
            }
        }
        return sb.toString()
    }

    private fun parseNumber_(): Number {
        val start = pos
        var c = peek_()
        if (c == '-') consume_()
        while (peek_().isDigit()) consume_()
        if (peek_() == '.') {
            consume_()
            while (peek_().isDigit()) consume_()
        }
        if (peek_() == 'e' || peek_() == 'E') {
            consume_()
            if (peek_() == '+' || peek_() == '-') consume_()
            while (peek_().isDigit()) consume_()
        }
        val s = text.substring(start, pos)
        return if (s.contains('.') || s.contains('e') || s.contains('E')) s.toDouble() else s.toLong()
    }
}

fun render(scenarioFile: java.io.File): String {
    val scenario = parseJson(scenarioFile.readText())
    val op = scenario["op"] as String

    val result = when (op) {
        "generate_ephemeral_keypair" -> {
            val kp = ShellCrypto.generateEphemeralKeyPair()
            val publicRaw = kp.public.encoded.copyOfRange(kp.public.encoded.size - 32, kp.public.encoded.size)
            jsonObj("public_key_raw" to JvmBase64.getUrlEncoder().withoutPadding().encodeToString(publicRaw).replace('+', '-').replace('/', '_').replace("=", ""),
                    "private_key_raw" to JvmBase64.getEncoder().withoutPadding().encodeToString(kp.private.encoded))
        }

        "encode_public_key" -> {
            val raw = JvmBase64.getUrlDecoder().decode(scenario["public_key_raw"] as String)
            val spki = X25519_SPKI_PREFIX + raw
            val publicKey = java.security.KeyFactory.getInstance("XDH").generatePublic(java.security.spec.X509EncodedKeySpec(spki))
            val encoded = ShellCrypto.encodePublicKey(publicKey)
            jsonObj("encoded" to encoded)
        }

        "decode_public_key" -> {
            val key = scenario["base64url_key"] as String
            try {
                val pk = ShellCrypto.decodePublicKey(key)
                val raw = pk.encoded.copyOfRange(pk.encoded.size - 32, pk.encoded.size)
                val b64 = JvmBase64.getUrlEncoder().withoutPadding().encodeToString(raw).replace('+', '-').replace('/', '_').replace("=", "")
                jsonObj("public_key_raw" to b64)
            } catch (e: Exception) {
                jsonObj("error" to e.message)
            }
        }

        "derive_aes_key" -> {
            val privatePkcs8 = JvmBase64.getUrlDecoder().decode(scenario["private_key_pkcs8"] as String)
            val publicRaw = JvmBase64.getUrlDecoder().decode(scenario["public_key_raw"] as String)
            val privateKey = java.security.KeyFactory.getInstance("X25519").generatePrivate(java.security.spec.PKCS8EncodedKeySpec(privatePkcs8))
            val publicKey = ShellCrypto.decodePublicKey(scenario["public_key_raw"] as String)
            val derived = ShellCrypto.deriveAesKey(privateKey, publicKey)
            val b64 = JvmBase64.getUrlEncoder().withoutPadding().encodeToString(derived).replace('+', '-').replace('/', '_').replace("=", "")
            jsonObj("derived_key" to b64)
        }

        "encrypt_decrypt" -> {
            val key = JvmBase64.getUrlDecoder().decode(scenario["key"] as String)
            val plaintext = (scenario["plaintext"] as String).toByteArray(StandardCharsets.UTF_8)
            val ciphertext = ShellCrypto.encrypt(key, plaintext)
            val decrypted = ShellCrypto.decrypt(key, ciphertext)
            jsonObj("ciphertext" to ciphertext, "decrypted" to String(decrypted, StandardCharsets.UTF_8))
        }

        "decrypt" -> {
            val key = JvmBase64.getUrlDecoder().decode(scenario["key"] as String)
            val ciphertextStr = scenario["ciphertext"] as String
            if (ciphertextStr == "PLACEHOLDER_WILL_BE_FILLED_BY_DRIVER") {
                val keyBytes = JvmBase64.getUrlDecoder().decode(scenario["key"] as String)
                val plaintext = "test".toByteArray(StandardCharsets.UTF_8)
                val ciphertext = ShellCrypto.encrypt(keyBytes, plaintext)
                val decrypted = ShellCrypto.decrypt(keyBytes, ciphertext)
                jsonObj("ciphertext" to ciphertext, "decrypted" to String(decrypted, StandardCharsets.UTF_8))
            } else {
                try {
                    val ciphertextBytes = JvmBase64.getUrlDecoder().decode(ciphertextStr)
                    val decrypted = ShellCrypto.decrypt(key, ciphertextStr)
                    jsonObj("decrypted" to String(decrypted, StandardCharsets.UTF_8))
                } catch (e: Exception) {
                    jsonObj("error" to e.message)
                }
            }
        }

        "sign" -> {
            val signature = ShellCrypto.sign(
                scenario["api_key"] as String,
                scenario["timestamp"] as Long,
                scenario["method"] as String,
                scenario["path"] as String,
                scenario["body_sha256"] as String,
                scenario["nonce"] as String,
                scenario["client_pub_key"] as String,
            )
            jsonObj("signature" to signature)
        }

        "sha256_hex" -> {
            val sha256 = ShellCrypto.sha256Hex((scenario["data"] as String).toByteArray(StandardCharsets.UTF_8))
            jsonObj("sha256" to sha256)
        }

        "response_signature" -> {
            val sessionKey = JvmBase64.getUrlDecoder().decode(scenario["session_key"] as String)
            val signature = ShellCrypto.responseSignature(sessionKey, (scenario["status"] as Long).toInt(), scenario["body"] as String)
            jsonObj("signature" to signature)
        }

        "verify_response_signature" -> {
            val sessionKey = JvmBase64.getUrlDecoder().decode(scenario["session_key"] as String)
            val signatureStr = scenario["signature"] as String
            val expected = if (signatureStr == "PLACEHOLDER_WILL_BE_FILLED_BY_DRIVER") {
                ShellCrypto.responseSignature(JvmBase64.getUrlDecoder().decode(scenario["session_key"] as String), (scenario["status"] as Long).toInt(), scenario["body"] as String)
            } else signatureStr
            val valid = ShellCrypto.verifyResponseSignature(
                JvmBase64.getUrlDecoder().decode(scenario["session_key"] as String),
                (scenario["status"] as Long).toInt(),
                scenario["body"] as String,
                expected
            )
            jsonObj("valid" to valid)
        }

        "generate_nonce" -> {
            val nonce = ShellCrypto.generateNonce()
            jsonObj("nonce" to nonce)
        }

        "base64_url_encode" -> {
            val input = (scenario["input"] as String).toByteArray(StandardCharsets.UTF_8)
            val encoded = Base64.encodeToString(input, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
            jsonObj("encoded" to encoded)
        }

        "base64_url_decode" -> {
            val decoded = Base64.decode(scenario["input"] as String, Base64.URL_SAFE or Base64.NO_WRAP)
            jsonObj("decoded" to String(decoded, StandardCharsets.UTF_8))
        }

        else -> error("unknown op $op")
    }
    return result
}

private fun ByteArray.toHexString(): String = joinToString("") { "%02x".format(it) }