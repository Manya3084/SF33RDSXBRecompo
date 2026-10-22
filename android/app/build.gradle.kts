// The Android app: SDL's activity (its Java, from the SDL checkout
// android/build-deps.sh made: -PsdlJava=PATH) and the game, built by
// android/build.sh into src/main/jniLibs.
plugins {
    id("com.android.application")
}

val sdlJava = providers.gradleProperty("sdlJava").orNull
    ?: error("-PsdlJava=PATH: SDL's android-project/app/src/main/java (android/build.sh passes it)")

android {
    namespace = "io.github.manya3084.sf33rdsxb"
    compileSdk = 37
    ndkVersion = "30.0.16248370"
    defaultConfig {
        applicationId = "io.github.manya3084.sf33rdsxb"
        minSdk = 26
        targetSdk = 37
        versionCode = 104
        versionName = "1.04"
        ndk {
            abiFilters += "arm64-v8a"
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
        }
    }
    lint {
        abortOnError = false
    }
}
