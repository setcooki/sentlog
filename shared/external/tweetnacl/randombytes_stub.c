// Tweetnacl declares `extern void randombytes(u8*, u64);` and uses
// it only inside crypto_box_keypair / crypto_sign_keypair. We never
// generate keys from the SDK — the integrator supplies the signing
// key via sentlog_set_signing_key — so the symbol must exist at
// link time but should never actually run.
//
// Calling this aborts to make the misuse loud rather than silently
// producing zero-keys. If the abort ever fires in production, a code
// path was added that calls crypto_*_keypair and that needs a real
// CSPRNG bound here.

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
typedef unsigned __int64 u64;
typedef unsigned char    u8;
#else
typedef unsigned long long u64;
typedef unsigned char      u8;
#endif

void randombytes(u8* /*buf*/, u64 /*len*/) {
    abort();
}

#ifdef __cplusplus
}  // extern "C"
#endif
