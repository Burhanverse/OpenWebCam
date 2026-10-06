package com.max.androidwebcam

import android.content.ActivityNotFoundException
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaFormat
import android.provider.Settings as AndroidSettings
import android.util.Size
import java.net.Inet4Address
import java.net.NetworkInterface

const val STREAM_FPS = 30

data class CameraInfo(
    val id: String,
    val label: String,
    val facing: Int,
    val sensorOrientation: Int,
    /** Sizes the camera can feed to an H.264 encoder at 30 fps, largest first. */
    val sizes: List<Size>,
)

object CameraCatalog {
    fun load(context: Context): List<CameraInfo> {
        val manager = context.getSystemService(CameraManager::class.java)
        val encoders = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { it.isEncoder && it.supportedTypes.any { t -> t.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) } }
            .map { it.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities }
        val counts = mutableMapOf<Int, Int>()

        return manager.cameraIdList.mapNotNull { id ->
            val chars = runCatching { manager.getCameraCharacteristics(id) }.getOrNull() ?: return@mapNotNull null
            val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return@mapNotNull null
            val sizes = map.getOutputSizes(MediaCodec::class.java).orEmpty()
                .filter { s ->
                    s.width % 2 == 0 && s.height % 2 == 0 &&
                        map.getOutputMinFrameDuration(MediaCodec::class.java, s) <= 1_000_000_000L / STREAM_FPS + 1 &&
                        encoders.any { vc -> runCatching { vc.areSizeAndRateSupported(s.width, s.height, STREAM_FPS.toDouble()) }.getOrDefault(false) }
                }
                .distinct()
                .sortedByDescending { it.width * it.height }
            if (sizes.isEmpty()) return@mapNotNull null

            val facing = chars.get(CameraCharacteristics.LENS_FACING) ?: CameraCharacteristics.LENS_FACING_EXTERNAL
            val name = when (facing) {
                CameraCharacteristics.LENS_FACING_BACK -> "Back camera"
                CameraCharacteristics.LENS_FACING_FRONT -> "Front camera"
                else -> "External camera"
            }
            val n = (counts[facing] ?: 0) + 1
            counts[facing] = n
            CameraInfo(
                id = id,
                label = if (n == 1) name else "$name $n",
                facing = facing,
                sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0,
                sizes = sizes,
            )
        }
    }

    /** The saved size if this camera supports it, else the supported size closest to it. */
    fun pickSize(camera: CameraInfo, width: Int, height: Int): Size =
        camera.sizes.firstOrNull { it.width == width && it.height == height }
            ?: camera.sizes.minBy { kotlin.math.abs(it.width * it.height - width * height) }

    fun sizeLabel(s: Size): String {
        val name = when (minOf(s.width, s.height)) {
            2160 -> " (4K)"
            1440 -> " (1440p)"
            1080 -> " (1080p)"
            720 -> " (720p)"
            480 -> " (480p)"
            else -> ""
        }
        return "${s.width} × ${s.height}$name"
    }
}

object Tether {
    private val prefixes = listOf("rndis", "ncm", "usb")

    /** The phone's IPv4 address on the USB tethering interface, or null if tethering is off. */
    fun address(): String? = runCatching {
        NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
            .filter { it.isUp && prefixes.any { p -> it.name.startsWith(p) } }
            .flatMap { it.inetAddresses.toList() }
            .firstOrNull { it is Inet4Address }?.hostAddress
    }.getOrNull()

    /** Opens the hotspot & tethering page; its location varies by vendor, so fall back gracefully. */
    fun openSettings(context: Context) {
        val candidates = listOf(
            Intent().setComponent(ComponentName("com.android.settings", "com.android.settings.TetherSettings")),
            Intent().setComponent(ComponentName("com.android.settings", "com.android.settings.Settings\$TetherSettingsActivity")),
            Intent(AndroidSettings.ACTION_WIRELESS_SETTINGS),
            Intent(AndroidSettings.ACTION_SETTINGS),
        )
        for (intent in candidates) {
            try {
                context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                return
            } catch (_: ActivityNotFoundException) {
            } catch (_: SecurityException) {
            }
        }
    }
}
