// Root project build. Plugin versions are aliased so that the library
// module pulls from a single source of truth.

plugins {
    alias(libs.plugins.android.library)             apply false
    alias(libs.plugins.kotlin.android)              apply false
    alias(libs.plugins.kotlin.serialization)        apply false
}
