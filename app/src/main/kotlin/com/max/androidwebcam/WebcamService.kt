package com.max.androidwebcam

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.OrientationEventListener
import android.view.Surface
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import java.io.BufferedOutputStream
import java.io.IOException
import java.io.OutputStream
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/**
 * Camera2 -> MediaCodec H.264 -> TCP (Protocol v2) to one PC client.
 *
 * Threads: camera thread owns the camera/encoder pipeline; one thread drains the
 * encoder; one accepts clients; each client gets a writer thread fed by a bounded
 * queue, so a slow or stalled PC never blocks the encoder (frames are dropped
 * until the next keyframe instead).
 */
class WebcamService : Service() {
    companion object {
        const val ACTION_APPLY = "com.max.androidwebcam.APPLY"
        private const val TAG = "OpenWebCam"
        private const val CHANNEL_ID = "openwebcam"
        private const val NOTIFICATION_ID = 42
        private const val QUEUE_PACKETS = 30
    }

    private class Packet(val type: Byte, val data: ByteArray)

    private val cameraThread = HandlerThread("Camera").apply { start() }
    private val cameraHandler = Handler(cameraThread.looper)
    private val serverExecutor = Executors.newSingleThreadExecutor()
    private val encoderExecutor = Executors.newSingleThreadExecutor()

    @Volatile private var running = false
    private var serverSocket: ServerSocket? = null

    // Client state, guarded by clientLock.
    private val clientLock = Any()
    private var client: Socket? = null
    private var queue: ArrayBlockingQueue<Packet>? = null
    private var dropUntilKeyframe = true

    // Pipeline state, owned by the camera thread (encoder/codecConfig also read by the drain thread).
    private lateinit var settings: Settings
    private var cameraInfo: CameraInfo? = null
    @Volatile private var size = Size(1280, 720)
    @Volatile private var rotation = 0
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    @Volatile private var encoder: MediaCodec? = null
    private var encoderSurface: Surface? = null
    @Volatile private var codecConfig: ByteArray? = null
    private var orientationListener: OrientationEventListener? = null
    @Volatile private var deviceDegrees = 0

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            StreamStatus.update { it.copy(running = false, error = "Camera permission is required") }
            stopSelf()
            return START_NOT_STICKY
        }
        if (!running) {
            createNotificationChannel()
            ServiceCompat.startForeground(this, NOTIFICATION_ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
            running = true
            settings = SettingsStore.load(this)
            StreamStatus.update { Status(running = true) }
            cameraHandler.post {
                try {
                    startServer()
                    startPipeline()
                } catch (t: Throwable) {
                    fail("Could not start: ${t.message}", t)
                }
            }
        } else if (intent?.action == ACTION_APPLY) {
            cameraHandler.post { applySettings() }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        running = false
        try { serverSocket?.close() } catch (_: IOException) {}
        synchronized(clientLock) { closeClientLocked() }
        cameraHandler.post { stopPipeline() }
        cameraThread.quitSafely()
        serverExecutor.shutdownNow()
        encoderExecutor.shutdownNow()
        StreamStatus.update { it.copy(running = false, pcAddress = null, description = null) }
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun fail(message: String, t: Throwable? = null) {
        Log.e(TAG, message, t)
        StreamStatus.update { it.copy(error = message) }
        stopSelf()
    }

    private fun notification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("OpenWebCam")
            .setContentText("Camera is streaming to your PC")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentIntent(open)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun createNotificationChannel() {
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Streaming", NotificationManager.IMPORTANCE_LOW),
        )
    }

    // ---- Settings & orientation (camera thread) ----

    private fun applySettings() {
        val old = settings
        settings = SettingsStore.load(this)
        if (settings.cameraId == old.cameraId && settings.width == old.width && settings.height == old.height) {
            updateOrientationListener()
            updateRotation()
            publishDescription()
        } else {
            stopPipeline()
            startPipeline()
        }
    }

    private fun computeRotation(): Int {
        val cam = cameraInfo ?: return 0
        val d = settings.orientation.deviceDegrees ?: deviceDegrees
        val signed = if (cam.facing == CameraCharacteristics.LENS_FACING_FRONT) -d else d
        return ((cam.sensorOrientation + signed) % 360 + 360) % 360
    }

    private fun updateRotation() {
        val r = computeRotation()
        if (r != rotation) {
            rotation = r
            sendInfo()
        }
    }

    private fun updateOrientationListener() {
        val wanted = settings.orientation == Orientation.AUTO
        if (wanted && orientationListener == null) {
            orientationListener = object : OrientationEventListener(this) {
                override fun onOrientationChanged(o: Int) {
                    if (o == ORIENTATION_UNKNOWN) return
                    val diff = ((o - deviceDegrees) % 360 + 360) % 360
                    if (diff in 60..300) {  // hysteresis: only switch well past the 45° boundary
                        deviceDegrees = ((o + 45) / 90 % 4) * 90
                        cameraHandler.post { updateRotation() }
                    }
                }
            }.also { it.enable() }
        } else if (!wanted) {
            orientationListener?.disable()
            orientationListener = null
        }
    }

    private fun publishDescription() {
        val cam = cameraInfo ?: return
        StreamStatus.update {
            it.copy(description = "${cam.label} · ${CameraCatalog.sizeLabel(size)} · ${settings.orientation.label}", error = null)
        }
    }

    // ---- Pipeline (camera thread) ----

    private fun startPipeline() {
        val cameras = CameraCatalog.load(this)
        val cam = cameras.firstOrNull { it.id == settings.cameraId }
            ?: cameras.firstOrNull { it.facing == CameraCharacteristics.LENS_FACING_BACK }
            ?: cameras.firstOrNull()
            ?: throw IllegalStateException("No usable camera")
        cameraInfo = cam
        size = CameraCatalog.pickSize(cam, settings.width, settings.height)
        Log.i(TAG, "Starting camera=${cam.id} ${cam.label} size=$size orientation=${settings.orientation}")

        startEncoder(size)
        updateOrientationListener()
        rotation = computeRotation()
        synchronized(clientLock) { dropUntilKeyframe = true }
        sendInfo()
        publishDescription()
        openCamera(cam.id)
    }

    private fun stopPipeline() {
        orientationListener?.disable()
        orientationListener = null
        try { session?.close() } catch (_: Throwable) {}
        session = null
        try { camera?.close() } catch (_: Throwable) {}
        camera = null
        val e = encoder
        encoder = null  // tells the drain loop to exit
        try { e?.stop() } catch (_: Throwable) {}
        try { e?.release() } catch (_: Throwable) {}
        try { encoderSurface?.release() } catch (_: Throwable) {}
        encoderSurface = null
        codecConfig = null
    }

    private fun startEncoder(size: Size) {
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, size.width, size.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateFor(size))
            setInteger(MediaFormat.KEY_FRAME_RATE, STREAM_FPS)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            setInteger(MediaFormat.KEY_LATENCY, 1)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
        }
        val name = MediaCodecList(MediaCodecList.REGULAR_CODECS).findEncoderForFormat(format)
            ?: throw IllegalStateException("No H.264 encoder supports ${size.width}x${size.height}")
        Log.i(TAG, "Encoder=$name bitrate=${bitrateFor(size)}")
        val codec = MediaCodec.createByCodecName(name)
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        encoderSurface = codec.createInputSurface()
        codec.start()
        encoder = codec
        encoderExecutor.execute { drainEncoder(codec) }
    }

    /** ~0.14 bits per pixel at 30 fps: 720p ≈ 3.9 Mbit/s, 1080p ≈ 8.7, 4K ≈ 35. */
    private fun bitrateFor(s: Size): Int =
        (s.width.toLong() * s.height * STREAM_FPS * 14 / 100).coerceIn(2_000_000L, 40_000_000L).toInt()

    private fun openCamera(id: String) {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            fail("Camera permission was revoked")
            return
        }
        val manager = getSystemService(CameraManager::class.java)
        manager.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                if (!running || cameraInfo?.id != id || encoderSurface == null || camera != null) {
                    device.close()  // stale: pipeline restarted meanwhile
                    return
                }
                camera = device
                createSession(device)
            }

            override fun onDisconnected(device: CameraDevice) {
                device.close()
                if (camera === device) fail("Camera was taken by another app")
            }

            override fun onError(device: CameraDevice, error: Int) {
                device.close()
                if (camera === device) fail("Camera error $error")
            }
        }, cameraHandler)
    }

    private fun createSession(device: CameraDevice) {
        val surface = encoderSurface ?: return
        val config = SessionConfiguration(
            SessionConfiguration.SESSION_REGULAR,
            listOf(OutputConfiguration(surface)),
            { command -> cameraHandler.post(command) },
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (camera !== device) {
                        s.close()
                        return
                    }
                    session = s
                    val request = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                        addTarget(surface)
                        set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                        fpsRange()?.let { set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
                    }.build()
                    s.setRepeatingRequest(request, null, cameraHandler)
                    Log.i(TAG, "Camera capture started")
                }

                override fun onConfigureFailed(s: CameraCaptureSession) {
                    if (camera === device) fail("Camera cannot stream ${size.width}x${size.height}")
                }
            },
        )
        device.createCaptureSession(config)
    }

    /** Steady 30 fps if the camera offers it; otherwise let auto-exposure choose. */
    private fun fpsRange(): Range<Int>? {
        val id = cameraInfo?.id ?: return null
        val ranges = getSystemService(CameraManager::class.java).getCameraCharacteristics(id)
            .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return null
        return ranges.filter { it.upper == STREAM_FPS }.maxByOrNull { it.lower }
    }

    // ---- Encoder output (drain thread) ----

    private fun drainEncoder(codec: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running && encoder === codec) {
            try {
                val index = codec.dequeueOutputBuffer(info, 10_000)
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    val f = codec.outputFormat
                    val csd0 = f.getByteBuffer("csd-0")
                    val csd1 = f.getByteBuffer("csd-1")
                    if (csd0 != null && csd1 != null) {
                        codecConfig = ensureAnnexB(ByteArray(csd0.remaining()).also { csd0.get(it) }) +
                            ensureAnnexB(ByteArray(csd1.remaining()).also { csd1.get(it) })
                    }
                } else if (index >= 0) {
                    val buffer = codec.getOutputBuffer(index)
                    if (buffer != null && info.size > 0) {
                        val data = ByteArray(info.size)
                        buffer.position(info.offset)
                        buffer.limit(info.offset + info.size)
                        buffer.get(data)
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                            codecConfig = ensureAnnexB(data)
                        } else {
                            sendVideo(data, info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0)
                        }
                    }
                    codec.releaseOutputBuffer(index, false)
                }
            } catch (t: Throwable) {
                if (running && encoder === codec) Log.e(TAG, "Encoder failed", t)
                return
            }
        }
    }

    private fun ensureAnnexB(b: ByteArray): ByteArray {
        val ok = b.size >= 4 && b[0].toInt() == 0 && b[1].toInt() == 0 &&
            (b[2].toInt() == 1 || (b[2].toInt() == 0 && b[3].toInt() == 1))
        check(ok) { "Encoder emitted non-Annex-B codec config" }
        return b
    }

    private fun requestSyncFrame() {
        try {
            encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (t: Throwable) {
            Log.w(TAG, "Sync-frame request failed", t)
        }
    }

    // ---- Client & sending ----

    private fun infoPacket() = Packet(Protocol.TYPE_INFO, Protocol.info(size.width, size.height, rotation))

    private fun sendInfo() = synchronized(clientLock) {
        val q = queue ?: return
        if (!q.offer(infoPacket())) {
            q.clear()
            q.offer(infoPacket())
            dropUntilKeyframe = true
            requestSyncFrame()
        }
    }

    private fun sendVideo(data: ByteArray, keyframe: Boolean) = synchronized(clientLock) {
        val q = queue ?: return
        if (dropUntilKeyframe) {
            if (!keyframe) return
            dropUntilKeyframe = false
            codecConfig?.let { q.offer(Packet(Protocol.TYPE_VIDEO, it)) }
        }
        if (!q.offer(Packet(Protocol.TYPE_VIDEO, data))) {
            // PC isn't keeping up: discard the backlog and restart from a keyframe.
            q.clear()
            q.offer(infoPacket())
            dropUntilKeyframe = true
            requestSyncFrame()
        }
    }

    private fun startServer() {
        val server = ServerSocket(Protocol.PORT)
        serverSocket = server
        serverExecutor.execute {
            Log.i(TAG, "Listening on port ${Protocol.PORT}")
            while (running) {
                val socket = try {
                    server.accept()
                } catch (e: IOException) {
                    if (running) Log.e(TAG, "Accept failed", e)
                    continue
                }
                socket.tcpNoDelay = true
                val q = ArrayBlockingQueue<Packet>(QUEUE_PACKETS)
                synchronized(clientLock) {
                    closeClientLocked()  // one PC at a time; newest wins
                    client = socket
                    queue = q
                    dropUntilKeyframe = true
                    q.offer(infoPacket())
                    requestSyncFrame()
                }
                val address = socket.inetAddress.hostAddress
                Log.i(TAG, "PC connected: $address")
                StreamStatus.update { it.copy(pcAddress = address) }
                thread(name = "Writer") { writeLoop(socket, q, BufferedOutputStream(socket.getOutputStream(), 256 * 1024)) }
            }
        }
    }

    private fun writeLoop(socket: Socket, q: ArrayBlockingQueue<Packet>, out: OutputStream) {
        try {
            while (running && !socket.isClosed) {
                val p = q.poll(500, TimeUnit.MILLISECONDS) ?: continue
                val len = p.data.size + 1
                out.write(len ushr 24)
                out.write(len ushr 16)
                out.write(len ushr 8)
                out.write(len)
                out.write(p.type.toInt())
                out.write(p.data)
                if (q.isEmpty()) out.flush()
            }
        } catch (_: IOException) {
        } catch (_: InterruptedException) {
        } finally {
            val wasCurrent = synchronized(clientLock) {
                (client === socket).also { if (it) closeClientLocked() }
            }
            try { socket.close() } catch (_: IOException) {}
            if (wasCurrent) {
                Log.i(TAG, "PC disconnected")
                StreamStatus.update { it.copy(pcAddress = null) }
            }
        }
    }

    private fun closeClientLocked() {
        try { client?.close() } catch (_: IOException) {}
        client = null
        queue = null
    }
}
