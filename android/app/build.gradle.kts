plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.opencam"
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.opencam"
        minSdk = 30
        targetSdk = 35
        // one version for both apps, set by release.ps1
        versionName = rootProject.file("../VERSION").readText().trim()
        versionCode = versionName!!.split(".").fold(0) { acc, s -> acc * 1000 + s.toInt() }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
