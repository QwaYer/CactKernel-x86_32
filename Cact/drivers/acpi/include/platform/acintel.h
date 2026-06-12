#ifndef __ACINTEL_H__
#define __ACINTEL_H__

#define ACPI_INTERNAL_VAR_XFACE
#define ACPI_EXTERNAL_VAR_XFACE

#define ACPI_CACHE_LINE_SIZE  64

#define acpi_restore_flags(flags)      __asm__ __volatile__("pushfl ; pop %0" : "=rm"(flags) :: "memory")
#define acpi_save_flags(flags)         __asm__ __volatile__("pushfl ; pop %0" : "=rm"(flags) :: "memory")
#define acpi_disable_int()             __asm__ __volatile__("cli" ::: "memory")
#define acpi_enable_int()              __asm__ __volatile__("sti" ::: "memory")

#define ACPI_ASM_MACROS
#define BREAKPOINT3                    __asm__ __volatile__("int3" ::: "memory")
#define BREAKPOINT

#endif
