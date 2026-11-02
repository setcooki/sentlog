# gtest fetched via FetchContent so contributors don't need a system
# install. Pinned to a recent stable tag.

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.18.0
    GIT_SHALLOW    TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(sentlog_tests
    ring_buffer_test.cpp
    config_test.cpp
    c_api_test.cpp
    vad_test.cpp
    pipeline_test.cpp
    text_match_test.cpp
    fingerprint_test.cpp
    prosody_test.cpp
    envelope_test.cpp
    signing_test.cpp
    ota_test.cpp
    hw_signer_test.cpp
    telemetry_sweep_test.cpp
    audio_encoder_test.cpp
    tsa_provider_test.cpp
    device_attestation_test.cpp
    core_hardening_test.cpp
    golden_test.cpp
)
target_link_libraries(sentlog_tests
    PRIVATE
        sentlog
        GTest::gtest_main
)
target_include_directories(sentlog_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/external
)

target_compile_definitions(sentlog_tests
    PRIVATE SENTLOG_REPO_ROOT="${CMAKE_SOURCE_DIR}/.."
)

include(GoogleTest)
gtest_discover_tests(sentlog_tests)
