cmake_minimum_required(VERSION 3.22)

project(sentlog_jni
    VERSION 0.1.0
    DESCRIPTION "Sentlog JNI bridge"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Path to the shared/ C++ core, relative to this CMakeLists.txt.
# android/sentlog/src/main/cpp/  ->  ../../../../../shared
set(SENTLOG_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../shared")
add_subdirectory("${SENTLOG_SHARED_DIR}" sentlog_shared_build)

add_library(sentlog_jni SHARED
    sentlog_jni.cpp
)
target_link_libraries(sentlog_jni
    PRIVATE
        sentlog
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
