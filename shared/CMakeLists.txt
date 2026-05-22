cmake_minimum_required(VERSION 3.22)

project(sentlog
    VERSION 0.1.0
    DESCRIPTION "Sentlog — universal device-first evidence and presence SDK (core)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SENTLOG_ENABLE_ASAN  "Enable AddressSanitizer"              OFF)
option(SENTLOG_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer"    OFF)
option(SENTLOG_ENABLE_MSAN  "Enable MemorySanitizer (nightly only)" OFF)
option(SENTLOG_BUILD_TESTS  "Build gtest suite"                    ON)

# Sanitisers are non-negotiable on every C++ PR.
if(SENTLOG_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(SENTLOG_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()
if(SENTLOG_ENABLE_MSAN)
    add_compile_options(-fsanitize=memory -fno-omit-frame-pointer)
    add_link_options(-fsanitize=memory)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor
)

# --- Third-party deps -------------------------------------------------
include(FetchContent)

# nlohmann/json — header-only JSON. MIT.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# --- The C ABI library -------------------------------------------------
add_library(sentlog STATIC
    src/c_api.cpp
    src/internal/ring_buffer.cpp
    src/internal/state.cpp
    src/internal/config.cpp
)
target_include_directories(sentlog
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(sentlog PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(sentlog PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
)

if(SENTLOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
