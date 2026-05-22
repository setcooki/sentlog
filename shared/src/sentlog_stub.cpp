// Phase 0 placeholder. Real implementations of the C ABI declared in
// shared/include/sentlog/sentlog.h land in Phase 1+. This file exists so
// CMake has at least one translation unit to build into a library.

#include "sentlog/sentlog.h"

extern "C" const char* sentlog_version(void) {
    return SENTLOG_VERSION_STRING;
}
