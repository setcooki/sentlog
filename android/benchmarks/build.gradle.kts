plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.benchmark)
}

android {
    namespace = "me.cooki.sentlog.benchmarks"
    compileSdk = 34

    defaultConfig {
        minSdk = 24
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        // androidx.benchmark requires a benchmark-specific runner.
        testInstrumentationRunner = "androidx.benchmark.junit4.AndroidBenchmarkRunner"
    }

    buildTypes {
        // androidx.benchmark mandates a `release`-style build type.
        // The benchmark gradle plugin injects its own benchmark
        // buildType derived from release. Library release builds are
        // non-debuggable by default; AGP's library DSL doesn't expose
        // isDebuggable on a release buildType so we don't set it.
        named("release") {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    sourceSets["androidTest"].kotlin.srcDir("src/androidTest/kotlin")

    testBuildType = "release"
}

dependencies {
    androidTestImplementation(project(":sentlog"))
    androidTestImplementation(libs.androidx.test.junit)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.benchmark.junit4)
    androidTestImplementation(libs.kotlinx.coroutines.core)
    androidTestImplementation(libs.kotlinx.coroutines.android)
}
