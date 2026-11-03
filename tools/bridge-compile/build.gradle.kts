buildscript {
    repositories {
        google()
        mavenCentral()
    }
    dependencies {
        classpath("com.android.tools.build:gradle:8.13.0")
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:2.4.21")
    }
}

// Normally supplied by the host app's React Native install.
subprojects {
    configurations.configureEach {
        resolutionStrategy.eachDependency {
            if (requested.group == "com.facebook.react" && requested.version.isNullOrEmpty()) {
                useVersion("0.88.1")
            }
        }
    }
}
