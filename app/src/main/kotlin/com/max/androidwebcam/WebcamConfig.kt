package com.max.androidwebcam

import android.content.Context
import java.nio.ByteBuffer
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update

/**
 * Wire protocol v2 (phone -> PC over TCP 5000), one message per packet:
 *   [u32 BE length of the rest][u8 type][payload]
 * TYPE_INFO payload: [u16 width][u16 height][u16 rotation] (BE). Rotation is the
 *   clockwise turn the PC must apply for an upright picture. Sent first on every
 *   connection and whenever size or rotation changes.
 * TYPE_VIDEO payload: one H.264 Annex-B access unit.
 */
object Protocol {
    const val PORT = 5000
    const val TYPE_VIDEO: Byte = 1
    const val TYPE_INFO: Byte = 2

    fun info(width: Int, height: Int, rotation: Int): ByteArray =
        ByteBuffer.allocate(6).putShort(width.toShort()).putShort(height.toShort())
            .putShort(rotation.toShort()).array()
}

/** deviceDegrees: phone rotation as reported by OrientationEventListener; null = follow the sensor. */
enum class Orientation(val label: String, val deviceDegrees: Int?) {
    AUTO("Auto (follow phone)", null),
    PORTRAIT("Portrait", 0),
    LANDSCAPE("Landscape", 270),
    LANDSCAPE_FLIPPED("Landscape (flipped)", 90),
    PORTRAIT_FLIPPED("Portrait (upside down)", 180),
}

data class Settings(val cameraId: String?, val width: Int, val height: Int, val orientation: Orientation)

object SettingsStore {
    private const val PREFS = "settings"

    fun load(context: Context): Settings {
        val p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        return Settings(
            cameraId = p.getString("cameraId", null),
            width = p.getInt("width", 1280),
            height = p.getInt("height", 720),
            orientation = runCatching { Orientation.valueOf(p.getString("orientation", null)!!) }
                .getOrDefault(Orientation.AUTO),
        )
    }

    fun save(context: Context, s: Settings) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString("cameraId", s.cameraId)
            .putInt("width", s.width)
            .putInt("height", s.height)
            .putString("orientation", s.orientation.name)
            .apply()
    }
}

/** Service -> UI status (same process). */
data class Status(
    val running: Boolean = false,
    val pcAddress: String? = null,
    val description: String? = null,
    val error: String? = null,
)

object StreamStatus {
    val state = MutableStateFlow(Status())
    fun update(f: (Status) -> Status) = state.update(f)
}
