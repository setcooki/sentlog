// Phase 0 stub Objective-C/C interop shim. Real bridge declarations
// (mirroring shared/include/sentlog/sentlog.h) come in Phase 1.
//
// This header is what Swift sees. It re-declares the subset of the C ABI
// that crosses into Swift via the C interop layer.

#ifndef SENTLOG_C_H
#define SENTLOG_C_H

#ifdef __cplusplus
extern "C" {
#endif

const char* sentlog_c_version(void);

#ifdef __cplusplus
}
#endif

#endif /* SENTLOG_C_H */
