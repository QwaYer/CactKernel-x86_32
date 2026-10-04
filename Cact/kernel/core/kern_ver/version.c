#include "version.h"

#ifndef CACT_VERSION
#define CACT_VERSION "unknown"
#endif

#ifndef CACT_COMMIT_HASH
#define CACT_COMMIT_HASH "no-git"
#endif

#ifndef CACT_COMPILER
#define CACT_COMPILER "unknown"
#endif

#ifndef CACT_BUILDER
#define CACT_BUILDER "unknown"
#endif

#define _STR(x) #x
#define STR(x)  _STR(x)

const char kernel_version[]     = STR(CACT_VERSION);
const char kernel_commit_hash[] = STR(CACT_COMMIT_HASH);
const char kernel_compiler[]    = STR(CACT_COMPILER);
const char kernel_builder[]     = STR(CACT_BUILDER);