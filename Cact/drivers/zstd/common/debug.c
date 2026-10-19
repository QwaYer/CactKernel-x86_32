/*
 * This module only hosts one global variable
 * which can be used to dynamically influence the verbosity of traces,
 * such as DEBUGLOG and RAWLOG
 */

#include "debug.h"

#if (DEBUGLEVEL>=2)
/* We only use this when DEBUGLEVEL>=2, but we get -Werror=pedantic errors if a
 * translation unit is empty. So remove this from Linux kernel builds, but
 * otherwise just leave it in.
 */
int g_debuglevel = DEBUGLEVEL;
#endif
