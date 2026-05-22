// Phase 0 placeholder. In Phase 1 this file will forward into the C++
// shared core through the C ABI declared in shared/include/sentlog/.

#include "sentlog_c/sentlog_c.h"

const char* sentlog_c_version(void) {
    return "0.1.0-phase0";
}
