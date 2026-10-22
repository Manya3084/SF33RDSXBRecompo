import java.io.File
import java.util.Properties

// The Android app: SDL's activity (its Java, from the SDL checkout
// android/build-deps.sh made: -PsdlJava=PATH) and the game, built by
// android/build.sh into src/main/jniLibs.
plugins {
    id("com.android.application")
}

val sdlJava = providers.gradleProperty("sdlJava").orNull
    ?: error("-PsdlJava=PATH: SDL's android-project/app/src/main/java (android/build.sh passes it)")

// A release is signed with the key -Psigning=PATH names (a properties file
// with storeFile, storePassword, keyAlias, keyPassword, kept out of the
// repository); without one it is signed with the debug key.
val signing = providers.gradleProperty("signing").orNull?.let { path ->
    Properties().apply { File(path).inputStream().use { load(it) } }
}

android {
    namespace = "io.github.manya3084.sf33rdsxb"
    compileSdk = 37
    ndkVersion = "30.0.16248370"
    defaultConfig {
        applicationId = "io.github.manya3084.sf33rdsxb"
        minSdk = 26
        targetSdk = 37
        versionCode = 110
        versionName = "1.1"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }
    signingConfigs {
        if (signing != null) {
            create("release") {
                storeFile = file(signing.getProperty("storeFile"))
                storePassword = signing.getProperty("storePassword")
                keyAlias = signing.getProperty("keyAlias")
                keyPassword = signing.getProperty("keyPassword")
            }
        }
    }
    sourceSets {
        getByName("main") {
            java.srcDir(sdlJava)
        }
    }
    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = if (signing != null) signingConfigs.getByName("release") else signingConfigs.getByName("debug")
        }
    }
    lint {
        abortOnError = false
    }
}
