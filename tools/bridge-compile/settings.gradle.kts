// Compiles the RN and Capacitor bridges' Android code against the SDK AAR.
// The published packages expect a host app; this stands in for one, on the
// AGP version Capacitor itself builds with. See run.sh.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url = uri(file("libs")) }
    }
}

rootProject.name = "sentlog-bridge-compile"

include(":sentlog-rn")
project(":sentlog-rn").projectDir = file("../../rn/android")
include(":sentlog-capacitor")
project(":sentlog-capacitor").projectDir = file("../../capacitor/android")
include(":capacitor-android")
project(":capacitor-android").projectDir =
    file("../../capacitor/node_modules/@capacitor/android/capacitor")
