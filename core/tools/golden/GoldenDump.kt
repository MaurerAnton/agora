import com.newoether.agora.api.util.tokens.bpe.BpeTokenCount
import com.newoether.agora.api.util.tokens.bpe.BpeVocabulary
import java.io.File
import java.util.Base64
import java.util.regex.Pattern

/**
 * Prints, for every base64-encoded text of a corpus, either its token count or its pieces.
 *
 * This is the Kotlin half of the parity stand: the same corpus is fed to the C++ port
 * (core/tools/dump_pieces.cpp) and the two outputs must be identical, which is what pins the port
 * to the real implementation rather than to a reading of it.
 *
 * Usage: GoldenDump <o200k_base.tiktoken> <count|pieces> <corpus.b64>
 */
fun main(args: Array<String>) {
    require(args.size == 3) { "usage: GoldenDump <vocab> <count|pieces> <corpus.b64>" }
    val mode = args[1]
    require(mode == "count" || mode == "pieces") { "mode must be count or pieces" }

    val vocabulary = File(args[0]).bufferedReader().useLines(BpeVocabulary::parseTiktoken)
    val pieces = Pattern.compile(BpeTokenCount.O200K_PATTERN)
    val decoder = Base64.getDecoder()
    val encoder = Base64.getEncoder()
    val out = StringBuilder()
    File(args[2]).forEachLine { line ->
        if (line.isEmpty()) return@forEachLine
        val text = String(decoder.decode(line), Charsets.UTF_8)
        when (mode) {
            "count" -> out.append(BpeTokenCount.of(text, vocabulary)).append('\n')
            "pieces" -> {
                val matcher = pieces.matcher(text)
                var first = true
                while (matcher.find()) {
                    if (!first) out.append(' ')
                    first = false
                    out.append(
                        encoder.encodeToString(
                            text.substring(matcher.start(), matcher.end()).toByteArray(Charsets.UTF_8)
                        )
                    )
                }
                out.append('\n')
            }
        }
    }
    print(out)
}
