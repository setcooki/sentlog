# Phase 0: a single sanity test that the library links. Real gtest suite
# (scoring, manifest verification, fingerprint, config validation, etc.)
# arrives with the modules in Phases 1–4.

add_executable(sentlog_smoke_test smoke_test.cpp)
target_link_libraries(sentlog_smoke_test PRIVATE sentlog)
add_test(NAME sentlog_smoke COMMAND sentlog_smoke_test)
