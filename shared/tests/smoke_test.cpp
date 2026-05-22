// Phase 0 smoke test: the library links and exports its version string.
// Real test coverage comes with the modules.

#include "sentlog/sentlog.h"
#include <cstring>
#include <cstdio>

int main(void) {
    const char* v = sentlog_version();
    if (v == nullptr || std::strlen(v) == 0) {
        std::fprintf(stderr, "sentlog_version() returned empty\n");
        return 1;
    }
    std::printf("sentlog %s\n", v);
    return 0;
}
