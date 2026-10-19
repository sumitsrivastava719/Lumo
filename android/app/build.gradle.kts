plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.fourbrains.ambientled"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.fourbrains.ambientled"
        minSdk = 26
        targetSdk = 36
        versionCode = 4
        versionName = "2.2"
    }

    // signing details come from keystore.properties (git-ignored); see keystore.properties.example
    val ks = java.util.Properties().apply {
        val f = rootProject.file("keystore.properties")
        if (f.exists()) f.inputStream().use { load(it) }
    }
    signingConfigs {
        create("release") {
            if (ks.isNotEmpty()) {
                storeFile = rootProject.file(ks.getProperty("storeFile"))
                storePassword = ks.getProperty("storePassword")
                keyAlias = ks.getProperty("keyAlias")
                keyPassword = ks.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (ks.isNotEmpty()) signingConfig = signingConfigs.getByName("release")
        }
        debug { if (ks.isNotEmpty()) signingConfig = signingConfigs.getByName("release") }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) }
}

// No dependencies: the app uses platform APIs only (Kotlin stdlib comes from the plugin).
