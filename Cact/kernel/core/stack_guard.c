#include <stdint.h>

uintptr_t __stack_chk_guard;

void stack_guard_init(void) {
    uint32_t lo, hi, lo2, hi2;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    for (volatile int i = 0; i < 100; i++) __asm__ volatile("nop");
    __asm__ volatile("rdtsc" : "=a"(lo2), "=d"(hi2));
    __stack_chk_guard = lo ^ hi ^ lo2 ^ hi2;
    __stack_chk_guard ^= (uintptr_t)&__stack_chk_guard;
    if (!__stack_chk_guard)
        __stack_chk_guard = 0xDEADBEEF;
}

//just kernel stack guard
__attribute__((noreturn))
void __stack_chk_fail(void) {
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}