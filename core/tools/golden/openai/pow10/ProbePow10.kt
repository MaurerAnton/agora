package openai.golden

/**
 * Dumps Math.pow(10.0, e) bit patterns for every exponent the numeric decode can
 * produce, so the C++ port can prove its pow matches the JVM bit for bit.
 * Output: "<exponent> <hex raw bits>" per line.
 */
fun main() {
    for (e in -400..400) {
        val v = Math.pow(10.0, e.toDouble())
        println("$e " + java.lang.Double.doubleToRawLongBits(v).toString(16))
    }
}
