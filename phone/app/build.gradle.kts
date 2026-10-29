plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Signed with a fixed key when CI has one (WADDLE_KEYSTORE: path, plus
// WADDLE_KEYSTORE_PASSWORD), so a new build installs over the old one; else
// with the debug key of the machine that built it.
val keystore = System.getenv("WADDLE_KEYSTORE")?.let { file(it) }?.takeIf { it.exists() }

android {
    namespace = "me.arnee.waddle"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "me.arnee.waddle"
        minSdk = 26
        targetSdk = 35
        versionCode = (System.getenv("WADDLE_VERSION_CODE") ?: "1").toInt()
        versionName = System.getenv("WADDLE_VERSION_NAME") ?: "dev"
        ndk { abiFilters += listOf("arm64-v8a") }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        if (keystore != null) {
            create("waddle") {
                storeFile = keystore
                storePassword = System.getenv("WADDLE_KEYSTORE_PASSWORD")
                keyAlias = "waddle"
                keyPassword = System.getenv("WADDLE_KEYSTORE_PASSWORD")
            }
        }
    }

    buildTypes {
        getByName("debug") {
            if (keystore != null) signingConfig = signingConfigs.getByName("waddle")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
