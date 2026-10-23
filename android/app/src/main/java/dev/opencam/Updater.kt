package dev.opencam

import android.app.Activity
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.provider.Settings
import org.json.JSONObject
import java.io.DataInputStream
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Checks GitHub Releases for a newer APK, rebuilds it from a delta against the installed APK when one matches
 * this version (else downloads it whole), checks it against the manifest's SHA-256 and hands it to the system
 * installer. Release layout: release.ps1.
 */
object Updater {
    private const val BASE = "https://github.com/RDK456/openCam/releases/latest/download/"
    const val ACTION_INSTALL = "dev.opencam.INSTALL"
    const val ACTION_STATUS = "dev.opencam.INSTALL_STATUS"

    private fun fetch(name: String): ByteArray? = try {
        val c = URL(BASE + name).openConnection() as HttpURLConnection
        c.connectTimeout = 15000
        c.readTimeout = 30000
        if (c.responseCode == 200) c.inputStream.use { it.readBytes() } else null
    } catch (e: IOException) {
        null
    }

    private fun code(v: String) = v.split(".").fold(0) { acc, s -> acc * 1000 + (s.toIntOrNull() ?: 0) }

    private fun sha(f: File) =
        MessageDigest.getInstance("SHA-256").digest(f.readBytes()).joinToString("") { "%02x".format(it) }

    private fun staged(ctx: Context) = File(ctx.cacheDir, "update.apk")

    /** Blocking. Returns the newer version once its APK is staged and verified, null when up to date; throws when
     *  GitHub can't be reached or the download doesn't verify. */
    fun check(ctx: Context): String? {
        val m = fetch("manifest.json")?.let { JSONObject(String(it)) } ?: throw IOException("Couldn't reach GitHub")
        val ver = m.optString("version")
        val cur = ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: return null
        if (code(ver) <= code(cur)) return null
        val want = m.optString("openCam.apk")
        val out = staged(ctx)
        if (out.exists() && sha(out) == want) return ver
        fetch("openCam.apk.from-$cur.delta")?.let { runCatching { patch(File(ctx.applicationInfo.sourceDir), it, out) } }
        if (!out.exists() || sha(out) != want) fetch("openCam.apk")?.let { out.writeBytes(it) }
        return if (out.exists() && sha(out) == want) ver else throw IOException("Download of $ver failed")
    }

    /** Inverse of BlockDelta in windows/app.cpp. */
    private fun patch(old: File, delta: ByteArray, out: File) {
        val d = DataInputStream(delta.inputStream())
        val magic = ByteArray(4).also { d.readFully(it) }
        require(String(magic) == "OCD1")
        RandomAccessFile(old, "r").use { src ->
            out.outputStream().buffered().use { o ->
                while (d.available() > 0) {
                    val op = d.readByte().toInt().toChar()
                    val v = d.readInt().toLong() and 0xFFFFFFFFL
                    val buf: ByteArray
                    if (op == 'C') {
                        buf = ByteArray(d.readInt())
                        src.seek(v)
                        src.readFully(buf)
                    } else {
                        buf = ByteArray(v.toInt())
                        d.readFully(buf)
                    }
                    o.write(buf)
                }
            }
        }
    }

    /**
     * Hands the staged APK to the system installer, which shows its own confirmation. Returns false when Android
     * first needs "install unknown apps" allowed for openCam (that settings page is opened instead).
     */
    fun install(a: Activity): Boolean {
        if (!a.packageManager.canRequestPackageInstalls()) {
            a.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.packageName)))
            return false
        }
        val pi = a.packageManager.packageInstaller
        val id = pi.createSession(PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL))
        pi.openSession(id).use { s ->
            s.openWrite("base.apk", 0, -1).use { o ->
                staged(a).inputStream().use { it.copyTo(o) }
                s.fsync(o)
            }
            val status = PendingIntent.getActivity(
                a, 2, Intent(a, MainActivity::class.java).setAction(ACTION_STATUS),
                PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
            s.commit(status.intentSender)
        }
        return true
    }
}
