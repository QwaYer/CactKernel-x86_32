#ifndef __ACENV_H__
#define __ACENV_H__

#define ACPI_SYSTEM_XFACE
#define ACPI_INTERFACE_XFACE
#define ACPI_INIT_FUNCTION

#if defined(__GNUC__) && !defined(__INTEL_COMPILER)
#define ACPI_GCC         1
#define ACPI_GNUC        1
#define COMPILER_DEPENDENT_INT64   long long
#define COMPILER_DEPENDENT_UINT64  unsigned long long
#define ACPI_INLINE       __inline__
#define ACPI_GET_FUNCTION_NAME     __func__
#define ACPI_UNUSED_VAR            __attribute__((unused))
#define ACPI_WARN_DEPRECATED       __attribute__((deprecated))
#define ACPI_DECLARE_DEPRECATED    __attribute__((deprecated))
#endif

#define ACPI_MACHINE_WIDTH         32
#define ACPI_X86                   1
#define ACPI_32BIT_PHYSICAL_ADDRESS

#define ACPI_USE_NATIVE_DIVIDE
#define ACPI_USE_NATIVE_MATH64
#define ACPI_USE_LOCAL_CACHE

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "actypes.h"
#include "platform/acintel.h"

#ifndef ACPI_SEMAPHORE_NULL
#define ACPI_SEMAPHORE_NULL             (-1)
#endif

#ifndef DEBUGGER_THREADING
#define DEBUGGER_THREADING              0
#endif

#define ACPI_FLUSH_CPU_CACHE()    __asm__ __volatile__("wbinvd" : : : "memory")

#define ACPI_ACQUIRE_GLOBAL_LOCK(Glptr, Acquired) \
    do { \
        uint32_t _new, _old; \
        _old = *(volatile uint32_t *)(Glptr); \
        (Acquired) = FALSE; \
        if (!(_old & 0x01)) { \
            _new = (_old | 0x01) + 0x02; \
        } else { \
            _new = _old | 0x01; \
        } \
        (Acquired) = (_new == _old); \
        *(volatile uint32_t *)(Glptr) = _new; \
    } while (0)

#define ACPI_RELEASE_GLOBAL_LOCK(Glptr, Pending) \
    do { \
        uint32_t _new, _old; \
        _old = *(volatile uint32_t *)(Glptr); \
        _new = _old & ~0x01; \
        (Pending) = (_new & 0x02) ? TRUE : FALSE; \
        *(volatile uint32_t *)(Glptr) = _new; \
    } while (0)

#endif
