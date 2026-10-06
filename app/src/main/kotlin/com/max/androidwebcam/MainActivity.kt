package com.max.androidwebcam

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings as AndroidSettings
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.OpenInNew
import androidx.compose.material.icons.rounded.AspectRatio
import androidx.compose.material.icons.rounded.CameraAlt
import androidx.compose.material.icons.rounded.ChevronRight
import androidx.compose.material.icons.rounded.Code
import androidx.compose.material.icons.rounded.Favorite
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.LocalCafe
import androidx.compose.material.icons.rounded.Person
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.ScreenRotation
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material.icons.rounded.Usb
import androidx.compose.material.icons.rounded.Videocam
import androidx.compose.material.icons.rounded.VideocamOff
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

private const val SOURCE_URL = "https://github.com/maxcodl/OpenWebCam"
private const val AUTHOR_URL = "https://github.com/maxcodl"

class MainActivity : ComponentActivity() {
    private var resumeCount by mutableIntStateOf(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        setContent { OpenWebCamTheme { MainScreen(resumeCount) } }
    }

    override fun onResume() {
        super.onResume()
        resumeCount++  // re-check permissions after returning from Settings
        // Hide the status bar; a swipe from the top shows it briefly.
        WindowCompat.getInsetsController(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.statusBars())
        }
    }
}

@Composable
private fun OpenWebCamTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val context = LocalContext.current
    val scheme = when {
        Build.VERSION.SDK_INT >= 31 -> if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> darkColorScheme()
        else -> lightColorScheme()
    }
    MaterialTheme(colorScheme = scheme, content = content)
}

private fun granted(context: Context, permission: String) =
    ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

private val wantedPermissions =
    if (Build.VERSION.SDK_INT >= 33) arrayOf(Manifest.permission.CAMERA, Manifest.permission.POST_NOTIFICATIONS)
    else arrayOf(Manifest.permission.CAMERA)

private fun openUrl(context: Context, url: String) =
    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))

@Composable
private fun MainScreen(resumeCount: Int) {
    val context = LocalContext.current
    val status by StreamStatus.state.collectAsState()
    var hasCamera by remember { mutableStateOf(granted(context, Manifest.permission.CAMERA)) }
    var askedOnce by rememberSaveable { mutableStateOf(false) }
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        hasCamera = granted(context, Manifest.permission.CAMERA)
        askedOnce = true
    }
    LaunchedEffect(resumeCount) {
        hasCamera = granted(context, Manifest.permission.CAMERA)
        if (!hasCamera && !askedOnce) permissionLauncher.launch(wantedPermissions)
    }

    val cameras = remember { CameraCatalog.load(context) }
    var settings by remember { mutableStateOf(SettingsStore.load(context)) }
    var tetherIp by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(Unit) {
        while (true) {
            tetherIp = withContext(Dispatchers.IO) { Tether.address() }
            delay(2000)
        }
    }

    fun apply(s: Settings) {
        settings = s
        SettingsStore.save(context, s)
        if (status.running) {
            context.startService(Intent(context, WebcamService::class.java).setAction(WebcamService.ACTION_APPLY))
        }
    }

    // Surface sets the default text colour; the status bar area stays clear while the
    // list scrolls edge-to-edge behind the navigation bar.
    Surface(Modifier.fillMaxSize()) {
    Column(
        Modifier
            .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
            .verticalScroll(rememberScrollState())
            .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Bottom))
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(Modifier.padding(start = 4.dp, top = 24.dp, bottom = 8.dp)) {
            Text("OpenWebCam", style = MaterialTheme.typography.headlineLarge, fontWeight = FontWeight.SemiBold)
            Text(
                "Your phone as a webcam for Windows",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (!hasCamera) {
            PermissionCard(askedOnce) { permissionLauncher.launch(wantedPermissions) }
        }
        StatusCard(
            status = status,
            enabled = hasCamera,
            onStart = {
                StreamStatus.update { it.copy(error = null) }
                ContextCompat.startForegroundService(context, Intent(context, WebcamService::class.java))
            },
            onStop = { context.stopService(Intent(context, WebcamService::class.java)) },
        )
        TetherCard(tetherIp) { Tether.openSettings(context) }
        SettingsCard(cameras, settings, ::apply)
        AboutCard()
        Footer()
    }
    }
}

@Composable
private fun SectionCard(
    containerColor: Color = MaterialTheme.colorScheme.surfaceContainer,
    content: @Composable () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.extraLarge,
        colors = CardDefaults.cardColors(containerColor = containerColor),
    ) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) { content() }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.primary)
}

/** Icon in a tinted circle, used as the leading element of cards and rows. */
@Composable
private fun Badge(icon: ImageVector, container: Color, content: Color, size: Int = 40) {
    Box(
        Modifier
            .size(size.dp)
            .background(container, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = null, tint = content, modifier = Modifier.size((size * 0.55f).dp))
    }
}

@Composable
private fun PermissionCard(askedOnce: Boolean, onRequest: () -> Unit) {
    val context = LocalContext.current
    // After a denial, Android may stop showing the prompt; then only Settings can grant it.
    val blocked = askedOnce && !(context as Activity).shouldShowRequestPermissionRationale(Manifest.permission.CAMERA)
    val scheme = MaterialTheme.colorScheme
    SectionCard(containerColor = scheme.errorContainer) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Badge(Icons.Rounded.Warning, scheme.error, scheme.onError)
            Spacer(Modifier.width(16.dp))
            Column {
                Text("Camera permission needed", style = MaterialTheme.typography.titleMedium, color = scheme.onErrorContainer)
                Text(
                    if (blocked) "Camera access was denied. Allow it under Permissions in the app's settings."
                    else "OpenWebCam needs the camera to stream video to your PC.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = scheme.onErrorContainer,
                )
            }
        }
        if (blocked) {
            Button(
                onClick = {
                    context.startActivity(
                        Intent(AndroidSettings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null)),
                    )
                },
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Open app settings") }
        } else {
            Button(onClick = onRequest, modifier = Modifier.fillMaxWidth()) { Text("Grant permission") }
        }
    }
}

@Composable
private fun StatusCard(status: Status, enabled: Boolean, onStart: () -> Unit, onStop: () -> Unit) {
    val scheme = MaterialTheme.colorScheme
    val streaming = status.running && status.pcAddress != null
    val (title, body) = when {
        !status.running -> "Not streaming" to "Start streaming, then pick the OpenWebCam camera on your PC."
        status.pcAddress == null -> "Waiting for PC" to "Open the OpenWebCam camera in any app on your PC."
        else -> "Streaming to PC" to "Connected to ${status.pcAddress}"
    }
    val container = if (streaming) scheme.primaryContainer else scheme.surfaceContainer
    val onContainer = if (streaming) scheme.onPrimaryContainer else scheme.onSurface
    SectionCard(containerColor = container) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            when {
                streaming -> Badge(Icons.Rounded.Videocam, scheme.primary, scheme.onPrimary, 52)
                status.running -> Badge(Icons.Rounded.Videocam, scheme.tertiaryContainer, scheme.onTertiaryContainer, 52)
                else -> Badge(Icons.Rounded.VideocamOff, scheme.surfaceContainerHighest, scheme.onSurfaceVariant, 52)
            }
            Spacer(Modifier.width(16.dp))
            Column {
                Text(title, style = MaterialTheme.typography.titleLarge, color = onContainer)
                Text(body, style = MaterialTheme.typography.bodyMedium, color = onContainer.copy(alpha = 0.8f))
            }
        }
        status.description?.takeIf { status.running }?.let {
            Text(it, style = MaterialTheme.typography.labelLarge, color = onContainer.copy(alpha = 0.8f))
        }
        status.error?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = scheme.error) }
        if (status.running) {
            FilledTonalButton(onClick = onStop, modifier = Modifier.fillMaxWidth().height(52.dp)) {
                ButtonContent(Icons.Rounded.Stop, "Stop streaming")
            }
        } else {
            Button(onClick = onStart, enabled = enabled, modifier = Modifier.fillMaxWidth().height(52.dp)) {
                ButtonContent(Icons.Rounded.PlayArrow, "Start streaming")
            }
        }
    }
}

@Composable
private fun RowScope.ButtonContent(icon: ImageVector, text: String) {
    Icon(icon, contentDescription = null, modifier = Modifier.size(ButtonDefaults.IconSize))
    Spacer(Modifier.width(ButtonDefaults.IconSpacing))
    Text(text, style = MaterialTheme.typography.titleSmall)
}

@Composable
private fun TetherCard(tetherIp: String?, onOpenSettings: () -> Unit) {
    val scheme = MaterialTheme.colorScheme
    val on = tetherIp != null
    SectionCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Badge(
                Icons.Rounded.Usb,
                if (on) scheme.secondaryContainer else scheme.surfaceContainerHighest,
                if (on) scheme.onSecondaryContainer else scheme.onSurfaceVariant,
            )
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Text("USB tethering", style = MaterialTheme.typography.titleMedium)
                Text(
                    if (on) "On · $tetherIp" else "Off",
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (on) scheme.primary else scheme.onSurfaceVariant,
                )
            }
        }
        if (!on) {
            Text(
                "Connect the USB cable and turn on USB tethering. It switches off whenever the cable is unplugged.",
                style = MaterialTheme.typography.bodyMedium,
                color = scheme.onSurfaceVariant,
            )
        }
        FilledTonalButton(onClick = onOpenSettings, modifier = Modifier.fillMaxWidth()) {
            ButtonContent(Icons.AutoMirrored.Rounded.OpenInNew, "Hotspot & tethering settings")
        }
    }
}

@Composable
private fun SettingsCard(cameras: List<CameraInfo>, settings: Settings, onChange: (Settings) -> Unit) {
    val camera = cameras.firstOrNull { it.id == settings.cameraId }
        ?: cameras.firstOrNull { it.facing == android.hardware.camera2.CameraCharacteristics.LENS_FACING_BACK }
        ?: cameras.firstOrNull()
    SectionCard {
        SectionTitle("Video")
        if (camera == null) {
            Text("No camera on this phone can stream H.264 video.", color = MaterialTheme.colorScheme.error)
            return@SectionCard
        }
        val size = CameraCatalog.pickSize(camera, settings.width, settings.height)
        Column {
            ChoiceRow(Icons.Rounded.CameraAlt, "Camera", camera.label, cameras, { it.label }, { it.id == camera.id }) { c ->
                val s = CameraCatalog.pickSize(c, settings.width, settings.height)
                onChange(settings.copy(cameraId = c.id, width = s.width, height = s.height))
            }
            RowDivider()
            ChoiceRow(Icons.Rounded.AspectRatio, "Resolution", CameraCatalog.sizeLabel(size), camera.sizes, CameraCatalog::sizeLabel, { it == size }) { s ->
                onChange(settings.copy(cameraId = camera.id, width = s.width, height = s.height))
            }
            RowDivider()
            ChoiceRow(Icons.Rounded.ScreenRotation, "Orientation", settings.orientation.label, Orientation.entries, { it.label }, { it == settings.orientation }) { o ->
                onChange(settings.copy(orientation = o))
            }
        }
        Text(
            "Changes apply live. The PC app chooses the final picture size; a portrait picture in a landscape app gets bars at the sides.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun RowDivider() = HorizontalDivider(Modifier.padding(start = 56.dp), color = MaterialTheme.colorScheme.outlineVariant)

/** Tappable row: leading icon, title + value, trailing chevron. */
@Composable
private fun NavRow(icon: ImageVector, title: String, value: String, onClick: () -> Unit) {
    val scheme = MaterialTheme.colorScheme
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Badge(icon, scheme.secondaryContainer, scheme.onSecondaryContainer)
        Spacer(Modifier.width(16.dp))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(value, style = MaterialTheme.typography.bodyMedium, color = scheme.onSurfaceVariant)
        }
        Icon(Icons.Rounded.ChevronRight, contentDescription = null, tint = scheme.onSurfaceVariant)
    }
}

@Composable
private fun <T> ChoiceRow(
    icon: ImageVector,
    title: String,
    current: String,
    options: List<T>,
    label: (T) -> String,
    isSelected: (T) -> Boolean,
    onSelect: (T) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    NavRow(icon, title, current) { open = true }
    if (open) {
        AlertDialog(
            onDismissRequest = { open = false },
            icon = { Icon(icon, contentDescription = null) },
            title = { Text(title) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    options.forEach { option ->
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clickable {
                                    onSelect(option)
                                    open = false
                                }
                                .padding(vertical = 10.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = isSelected(option), onClick = null)
                            Spacer(Modifier.width(12.dp))
                            Text(label(option), style = MaterialTheme.typography.bodyLarge)
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { open = false }) { Text("Close") } },
        )
    }
}

@Composable
private fun AboutCard() {
    val context = LocalContext.current
    SectionCard {
        SectionTitle("About")
        Column {
            NavRow(Icons.Rounded.Code, "Source code", "github.com/maxcodl/OpenWebCam") { openUrl(context, SOURCE_URL) }
            RowDivider()
            NavRow(Icons.Rounded.Person, "Author", "Max · @maxcodl") { openUrl(context, AUTHOR_URL) }
            RowDivider()
            Row(Modifier.padding(vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                Badge(Icons.Rounded.Info, MaterialTheme.colorScheme.secondaryContainer, MaterialTheme.colorScheme.onSecondaryContainer)
                Spacer(Modifier.width(16.dp))
                Column {
                    Text("Version", style = MaterialTheme.typography.bodyLarge)
                    Text(BuildConfig.VERSION_NAME, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun Footer() {
    val color = MaterialTheme.colorScheme.onSurfaceVariant
    val style = MaterialTheme.typography.bodyMedium
    Row(
        Modifier
            .fillMaxWidth()
            .padding(top = 8.dp, bottom = 16.dp),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("Made with ", style = style, color = color)
        Icon(Icons.Rounded.Favorite, contentDescription = "love", tint = Color(0xFFE5484D), modifier = Modifier.size(16.dp))
        Text(" and ", style = style, color = color)
        Icon(Icons.Rounded.LocalCafe, contentDescription = "coffee", tint = Color(0xFFB07A4F), modifier = Modifier.size(16.dp))
        Text(" by Max", style = style, color = color)
    }
}
