#include <stdint.h>

uintptr_t __stack_chk_guard = 0xDEADC0DE;

//just kernel stack guard
__attribute__((noreturn))
void __stack_chk_fail(void) {
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}