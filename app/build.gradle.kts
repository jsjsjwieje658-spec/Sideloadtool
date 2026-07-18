/*
 * app/build.gradle.kts
 *
 * FIX v18:
 *   - Thêm armeabi-v7a vào abiFilters để hỗ trợ thiết bị Android 32-bit cũ.
 *   - extractNativeLibs=true (AndroidManifest) đảm bảo .so được giải nén vào
 *     /data/app/.../lib/<abi>/ — cần thiết để System.loadLibrary() tìm thấy.
 *   - useLegacyPackaging=true giữ .so không bị nén trong APK.
 *   - Tách cấu hình Chaquopy ra file Groovy riêng (python-config.gradle) vì
 *     Kotlin DSL không giải quyết được dynamic extension `python {}`.
 */
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("com.chaquo.python")
}

android {
    namespace = "com.superalpha.sideload"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.superalpha.sideload"
        minSdk = 26
        targetSdk = 34
        versionCode = 5
        versionName = "1.1.0"

        // FIX: Thêm x86_64 cho emulator và arm64-v8a cho thiết bị thật
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cppFlags("")
                arguments(
                    "-DANDROID_STL=c++_shared",
                    // Truyền thư mục prebuilt vào CMake để tự detect mode
                    "-DPREBUILT_ROOT=${projectDir}/src/main/cpp/prebuilt"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    ndkVersion = "25.2.9519653"

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
        debug {
            isDebuggable = true
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
        compose = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
            excludes += "META-INF/versions/9/OSGI-INF/MANIFEST.MF"
        }
        // FIX: useLegacyPackaging=true → .so được đóng gói không nén,
        // Android extract ra /data/app/.../lib/ khi cài đặt,
        // System.loadLibrary("sideloadnative") tìm thấy đúng.
        jniLibs {
            useLegacyPackaging = true
        }
    }

    androidResources {
        noCompress += listOf(
            "zsign_deps/libssl.so.3",
            "zsign_deps/libcrypto.so.3",
            "zsign_deps/libc++_shared.so"
        )
    }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")
    implementation("androidx.navigation:navigation-compose:2.8.0")

    val composeBom = platform("androidx.compose:compose-bom:2024.09.03")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")

    // BouncyCastle — dùng bởi CertHelper.kt
    implementation("org.bouncycastle:bcprov-jdk18on:1.78.1")
    implementation("org.bouncycastle:bcpkix-jdk18on:1.78.1")

    // OkHttp — dùng cho listAnisetteServers() trong PythonBridge.kt
    implementation("com.squareup.okhttp3:okhttp:4.12.0")

    debugImplementation("androidx.compose.ui:ui-tooling")
}

// Cấu hình Chaquopy pip đặt trong file Groovy riêng vì Kotlin DSL không thể
// giải quyết extension `python {}` mà Chaquopy thêm động vào DefaultConfig.
apply(from = "python-config.gradle")
