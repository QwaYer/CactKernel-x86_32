#include "version.h"

#ifndef LUX_VERSION
#define LUX_VERSION "unknown"
#endif

#ifndef LUX_BUILD_TIME
#define LUX_BUILD_TIME __DATE__ " " __TIME__
#endif

#ifndef LUX_COMMIT_HASH
#define LUX_COMMIT_HASH "no-git"
#endif

#define _STR(x) #x
#define STR(x)  _STR(x)

const char kernel_version[]     = STR(LUX_VERSION);
const char kernel_build_time[]  = STR(LUX_BUILD_TIME);
const char kernel_commit_hash[] = STR(LUX_COMMIT_HASH);