cmake_minimum_required(VERSION 3.22)

project(sentlog_jni
    VERSION 0.1.0
    DESCRIPTION "Sentlog JNI bridge"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Path to the shared/ C++ core, relative to this CMakeLists.txt.
# android/sentlog/src/main/cpp/  ->  ../../../../../shared
set(SENTLOG_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../shared")
add_subdirectory("${SENTLOG_SHARED_DIR}" sentlog_shared_build)

# whisper.cpp — Phase 2 ASR. Pulled in via FetchContent (consistent
# with how googletest comes in on the shared/ side). The vendored
# build is CPU-only — Vulkan / GPU paths are deferred follow-ups. The
# model file (.bin) is *not* fetched here; the Kotlin runner loads it
# from assets at runtime and gracefully no-ops when absent.
include(FetchContent)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "")
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "")
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "")
set(GGML_OPENMP            OFF CACHE BOOL "")   # OpenMP isn't on the Android NDK by default
FetchContent_Declare(
    whisper_cpp
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        v1.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(whisper_cpp)

add_library(sentlog_jni SHARED
    sentlog_jni.cpp
    whisper_jni.cpp
)
target_link_libraries(sentlog_jni
    PRIVATE
        sentlog
        whisper
        android
        log
)
set_target_properties(sentlog_jni PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Strip release builds aggressively; debug builds keep symbols for ndk-stack.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(sentlog_jni PRIVATE "-Wl,--gc-sections,--strip-all")
endif()
