plugins {
    id("com.android.application")
}

android {
    namespace = "foundation.sm64"
    compileSdk = 34

    // CI pins the NDK to the version present on the runner image; unset locally.
    System.getenv("FOUNDATION_ANDROID_NDK_VERSION")?.takeIf { it.isNotBlank() }?.let { ndkVersion = it }

    defaultConfig {
        applicationId = "foundation.sm64"
        minSdk = 33
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-33",
                    "-DFOUNDATION_RHIVULKAN_VALIDATION_LAYER=OFF",
                    "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384",
                    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384",
                    "-Wno-deprecated"
                )
                // The game itself; Foundation and SDL3 are pulled in as its
                // dependencies (SDL3 is built shared on Android).
                targets("sm64_foundation")
            }
        }

        ndk {
            abiFilters.add("arm64-v8a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.26.0+"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    // Shaders are read straight out of the APK's assets -- keep them uncompressed.
    androidResources {
        noCompress.add("spv")
    }
}

// Shaders (and the rest of Data/) are produced by the native build, which Gradle
// otherwise races: merge*Assets has to wait for externalNativeBuild*.
afterEvaluate {
    tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }.configureEach {
        val variantName = name.removePrefix("merge").removeSuffix("Assets")
        val nativeVariant = variantName.removeSuffix("AndroidTest").removeSuffix("UnitTest")
        tasks.findByName("externalNativeBuild$nativeVariant")?.let { dependsOn(it) }
    }
}
