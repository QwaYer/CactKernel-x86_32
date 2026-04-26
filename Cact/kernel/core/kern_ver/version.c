#include "version.h"

#ifndef CACT_VERSION
#define CACT_VERSION "unknown"
#endif

#ifndef CACT_BUILD_TIME
#define CACT_BUILD_TIME __DATE__ " " __TIME__
#endif

#ifndef CACT_COMMIT_HASH
#define CACT_COMMIT_HASH "no-git"
#endif

#define _STR(x) #x
#define STR(x)  _STR(x)

const char kernel_version[]     = STR(CACT_VERSION);
const char kernel_build_time[]  = STR(CACT_BUILD_TIME);
const char kernel_commit_hash[] = STR(CACT_COMMIT_HASH);