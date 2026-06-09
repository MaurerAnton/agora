plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
}

import java.util.Properties

val keystoreProperties = Properties()
val keystorePropertiesFile = rootProject.file("local.properties")
if (keystorePropertiesFile.exists()) {
    keystoreProperties.load(keystorePropertiesFile.reader())
}

android {
    namespace = "com.newoether.agora"
    compileSdk {
        version = release(36)
    }

    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "com.newoether.agora"
        minSdk = 24
        targetSdk = 36
        versionCode = 13
        versionName = "1.2.0"


        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf("-DANDROID_STL=c++_shared")
                targets += listOf("agora_llama")
            }
        }
    }

    ksp {
        arg("room.schemaLocation", "$projectDir/schemas")
    }

    signingConfigs {
        create("release") {
            storeFile = file(keystoreProperties.getProperty("storeFile", "."))
            storePassword = keystoreProperties.getProperty("storePassword", "")
            keyAlias = keystoreProperties.getProperty("keyAlias", "")
            keyPassword = keystoreProperties.getProperty("keyPassword", "")
        }
    }

    val hasKeystore = keystoreProperties.getProperty("storeFile", ".").let { it != "." }
    val releaseSigning = if (hasKeystore) signingConfigs.getByName("release") else signingConfigs.getByName("debug")

    buildTypes {
        release {
            signingConfig = releaseSigning
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    dependenciesInfo {
        includeInApk = false
        includeInBundle = false
    }
    buildFeatures {
        compose = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }
}

tasks.register<Copy>("copyReleaseApk") {
    from("build/outputs/apk/release")
    into("release")
    include("*.apk")
}

tasks.register<Copy>("copyReleaseBundle") {
    from("build/outputs/bundle/release")
    into("release")
    include("*.aab")
}

afterEvaluate {
    tasks.named("assembleRelease") {
        finalizedBy("copyReleaseApk")
    }
    tasks.named("bundleRelease") {
        finalizedBy("copyReleaseBundle")
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.core.splashscreen)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation("androidx.compose.foundation:foundation")
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.androidx.room.runtime)
    implementation(libs.androidx.room.ktx)
    ksp(libs.androidx.room.compiler)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.compose.markdown)
    implementation(libs.coil.compose)
    implementation(libs.jlatexmath.android)
    implementation(libs.media3.exoplayer)
    implementation(libs.media3.ui)
    implementation(libs.okhttp)
    implementation(libs.material.color.utilities)
    implementation(libs.lottie.compose)
    debugImplementation(libs.androidx.compose.ui.tooling)
}

tasks.whenTaskAdded {
    if (name.contains("ArtProfile") || name.contains("BaselineProfile") || name.contains("baselineProfile")) {
        enabled = false
    }
    if (name.contains("StripDebugSymbols") || name.contains("MergeNativeDebugMetadata")) {
        enabled = false
    }
}