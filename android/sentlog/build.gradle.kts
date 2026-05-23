plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "me.cooki.sentlog"
    compileSdk = 34

    defaultConfig {
        // PCM_FLOAT capture requires API 23; minSdk 24 keeps us above
        // a comfortable floor while still covering ~99% of devices.
        minSdk = 24

        ndk {
            // 64-bit only. armeabi-v7a (32-bit ARM) is intentionally
            // omitted — modern Android devices are all 64-bit and
            // dropping it removes a real chunk of build time.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DSENTLOG_BUILD_TESTS=OFF",
                )
                cppFlags += listOf("-std=c++17")
            }
        }

        consumerProguardFiles("consumer-rules.pro")
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        debug {
            // ASan / UBSan stay off in debug AAR builds because they
            // change the runtime ABI and integrators don't expect
            // that. The CI shared/ job runs the sanitiser build
            // independently against the C++ core.
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Debug")
                }
            }
        }
        release {
            isMinifyEnabled = false
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Release")
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            // Points at the JNI bridge's CMakeLists, which in turn
            // pulls in shared/CMakeLists.txt as a subdirectory.
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    sourceSets["main"].kotlin.srcDir("src/main/kotlin")
    sourceSets["test"].kotlin.srcDir("src/test/kotlin")
    sourceSets["androidTest"].kotlin.srcDir("src/androidTest/kotlin")

    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            isReturnDefaultValues = true
        }
    }

    publishing {
        singleVariant("release") {
            withSourcesJar()
        }
    }
}

dependencies {
    implementation(libs.kotlinx.coroutines.core)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    // ONNX Runtime Mobile — runs Silero VAD and (Phase 1e2) AASIST-L
    // on-device. Ships its own JNI .so per ABI; the prebuilt AAR
    // covers arm64-v8a + x86_64 (matches our abiFilters above).
    implementation(libs.onnxruntime.android)

    testImplementation(libs.junit)
    testImplementation(libs.robolectric)
    testImplementation(libs.kotlinx.coroutines.core)

    androidTestImplementation(libs.androidx.test.junit)
    androidTestImplementation(libs.androidx.test.runner)
}
