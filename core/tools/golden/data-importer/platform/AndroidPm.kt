// Stand-local android.content.pm.PackageManager: the manifest's app_version source.
package android.content.pm

class PackageManager(private val versionName: String?) {
    data class PackageInfo(val versionName: String?)

    fun getPackageInfo(packageName: String, flags: Int): PackageInfo = PackageInfo(versionName)
}
