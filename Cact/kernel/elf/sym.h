#ifndef SYM_H
#define SYM_H

#include <stdint.h>

const char* sym_resolve_addr(uint32_t addr, uint32_t* offset);

#endif
