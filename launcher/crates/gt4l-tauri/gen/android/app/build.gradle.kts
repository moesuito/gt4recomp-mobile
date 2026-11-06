import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("rust")
}

val tauriProperties = Properties().apply {
    val propFile = file("tauri.properties")
    if (propFile.exists()) {
        propFile.inputStream().use { load(it) }
    }
}

// TASK-180: the release signing key stays outside the repository (the owner's private/ folder). GT4_ANDROID_SIGNING names
// a properties file with storeFile, storePassword, keyAlias and keyPassword; without it a release APK is left unsigned.
val signingProperties: Properties? = System.getenv("GT4_ANDROID_SIGNING")?.let { path ->
    val signingFile = file(path)
    if (!signingFile.exists()) throw GradleException("GT4_ANDROID_SIGNING names a missing file: $path")
    Properties().apply { signingFile.inputStream().use { load(it) } }
}

android {
    compileSdk = 36
    namespace = "io.github.moesuito.gt4recomp"
    // The NDK the game library is built with (android/native); AGP strips the staged libraries with it.
    ndkVersion = "26.1.10909125"
    signingConfigs {
        if (signingProperties != null) {
            create("release") {
                storeFile = file(signingProperties.getProperty("storeFile"))
                storePassword = signingProperties.getProperty("storePassword")
                keyAlias = signingProperties.getProperty("keyAlias")
                keyPassword = signingProperties.getProperty("keyPassword")
            }
        }
    }
    defaultConfig {
        manifestPlaceholders["usesCleartextTraffic"] = "false"
        applicationId = "io.github.moesuito.gt4recomp"
        // Android 10: Vulkan 1.1+ and AAudio on every device (the game library is built for API 29).
        minSdk = 29
        targetSdk = 36
        // TASK-180 test3: the version Android shows comes from the same `GT4_RELEASE_LABEL` the launcher library uses
        // (`gt4l-core/build.rs`: the crate version + "-" + label), so the APK, the home footer and the logs say the same
        // thing. The tauri CLI rewrites tauri.properties at every build with its own last values, so the version is
        // taken from the crate version part of that file and the label from the environment; the code from the
        // environment too (3 for this test build).
        val releaseLabel = (System.getenv("GT4_RELEASE_LABEL") ?: "").trim()
        val crateVersion = tauriProperties.getProperty("tauri.android.versionName", "1.3.0").substringBefore("-")
        versionCode = (System.getenv("GT4_ANDROID_VERSION_CODE") ?: "3").toInt()
        versionName = if (releaseLabel.isEmpty()) crateVersion else "$crateVersion-$releaseLabel"
    }
    buildTypes {
        getByName("debug") {
            manifestPlaceholders["usesCleartextTraffic"] = "true"
            isDebuggable = true
            isJniDebuggable = true
            isMinifyEnabled = false
            packaging {                jniLibs.keepDebugSymbols.add("*/arm64-v8a/*.so")
                jniLibs.keepDebugSymbols.add("*/armeabi-v7a/*.so")
                jniLibs.keepDebugSymbols.add("*/x86/*.so")
                jniLibs.keepDebugSymbols.add("*/x86_64/*.so")
            }
        }
        getByName("release") {
            // No shrinking: SDL3 and the Tauri plugin reach their Java and Kotlin code through JNI and reflection, and
            // the APK's size is the native libraries.
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release")
        }
    }
    packaging {
        jniLibs {
            // Stored uncompressed and page-aligned in the APK: loaded straight from it, 16 KB pages included.
            useLegacyPackaging = false
            // The game library comes staged without line tables but with its symbol table (android/stage-native-libs.cmd):
            // a tombstone then names the crashing function.
            keepDebugSymbols.add("**/libgt4.so")
            // TASK-180 test5: the Spec II library, packaged beside v2.00's and loaded only when that game runs.
            keepDebugSymbols.add("**/libgt4spec2.so")
        }
    }
    kotlinOptions {
        jvmTarget = "1.8"
    }
    sourceSets {
        getByName("main") {
            // TASK-185: the virtual pad's default layout ships byte-identical from the repository
            // (`android/virtual-pad/default-layout.json`), so the app and the game read one file. Its folder is the APK's
            // assets root: the game activity opens "default-layout.json" (GameActivity.loadVirtualPad).
            assets.srcDir(file("../../../../../../android/virtual-pad"))
        }
    }
    buildFeatures {
        buildConfig = true
    }
}

rust {
    rootDirRel = "../../../"
}

dependencies {
    // TASK-180: SDL3 3.4.18, the official Android package (libsdl-org release-3.4.18, git 829a65d7): SDLActivity and its
    // Java classes. Staged by android/stage-native-libs.cmd with libSDL3.so, libgt4.so and libc++_shared.so (none committed).
    implementation(files("libs/SDL3-3.4.18.aar"))
    implementation("androidx.webkit:webkit:1.14.0")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.activity:activity-ktx:1.10.1")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.lifecycle:lifecycle-process:2.10.0")
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.1.4")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.0")
}

apply(from = "tauri.build.gradle.kts")