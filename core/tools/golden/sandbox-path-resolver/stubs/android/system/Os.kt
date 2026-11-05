// Platform substitute for android.system.Os. The traced path-resolution scenarios never assert
// on the real filesystem metadata these helpers probe (chmod/stat/rename run on the stand's
// temp files only), so the calls are inert here.
package android.system

class StructStat {
    @JvmField
    var st_mode: Int = 0
}

object Os {
    @JvmStatic
    fun stat(path: String): StructStat = StructStat()

    @JvmStatic
    fun chmod(path: String, mode: Int) {}

    @JvmStatic
    fun rename(oldPath: String, newPath: String) {}
}
