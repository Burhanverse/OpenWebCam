import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Release signing: keystore.properties (local, gitignored) or SIGNING_* env vars (CI).
// Without them, release builds fall back to the debug key.
val keystoreProps = Properties().apply {
    rootProject.file("keystore.properties").takeIf { it.exists() }?.inputStream()?.use { load(it) }
}
fun signing(key: String, env: String): String? = keystoreProps.getProperty(key) ?: System.getenv(env)
val releaseStoreFile = signing("storeFile", "SIGNING_STORE_FILE")?.takeIf { it.isNotBlank() }

android {
    namespace = "com.max.androidwebcam"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.max.androidwebcam"
        minSdk = 29
        targetSdk = 35
        // CI passes -PversionName/-PversionCode from the release tag.
        versionCode = (findProperty("versionCode") as String?)?.toInt() ?: 3
        versionName = (findProperty("versionName") as String?) ?: "0.2.0"
    }

    signingConfigs {
        if (releaseStoreFile != null) {
            create("release") {
                storeFile = rootProject.file(releaseStoreFile)
                storePassword = signing("storePassword", "SIGNING_STORE_PASSWORD")
                keyAlias = signing("keyAlias", "SIGNING_KEY_ALIAS")
                keyPassword = signing("keyPassword", "SIGNING_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            // Shrinks the icon library and Compose to what the app uses.
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        buildConfig = true
        compose = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation(platform("androidx.compose:compose-bom:2026.03.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended:1.7.8")
}
