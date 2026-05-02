[BITS 32]

global gdt_flush
global tss_flush

; void gdt_flush(uint32_t gdt_ptr)
; Loads new GDT and updates segment registers, then far jumps to reload CS
gdt_flush:
    mov eax, [esp + 4]      ; eax = pointer to GDT descriptor (limit + base)
    lgdt [eax]              ; Load GDT into GDTR

    mov ax, 0x10            ; Data segment selector (index 2, RPL 0)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:flush_cs       ; Code segment selector (index 1, RPL 0)
flush_cs:
    ret

; void tss_flush(void)
; Loads Task State Segment selector into TR register
tss_flush:
    mov ax, 0x28            ; TSS segment selector (index 5, RPL 0)
    ltr ax                  ; Load Task Register
    ret