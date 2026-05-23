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
        // androidx.benchmark mandates a `release`-style "benchmark"
        // build type. The benchmark plugin wires this up automatically;
        // we just need to make sure release-equivalent settings apply.
        named("release") {
            isMinifyEnabled = false
            // Disable debugging in the test app — debug builds skew
            // timing badly.
            isDebuggable = false
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
