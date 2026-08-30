[bits 32]

; I/O port access primitives — cdecl calling convention
; Each function takes port number as first argument, value (for out) as second.
; Return value in EAX for in functions.

global outl
global inl
global outb
global inb
global inw
global outw 

; void outl(uint16_t port, uint32_t value)
outl:
    mov edx, [esp + 4]   ; port
    mov eax, [esp + 8]   ; value
    out dx, eax
    ret

; uint32_t inl(uint16_t port)
inl:
    mov edx, [esp + 4]   ; port
    in eax, dx            ; 32-bit read — EAX fully overwritten
    ret

; void outb(uint16_t port, uint8_t value)
outb:
    mov edx, [esp + 4]   ; port
    mov al, [esp + 8]    ; value (low byte only)
    out dx, al
    ret

; uint8_t inb(uint16_t port)
inb:
    mov edx, [esp + 4]   ; port
    xor eax, eax          ; zero EAX so upper bits are clean
    in al, dx             ; 8-bit read
    ret

; uint16_t inw(uint16_t port)
inw:
    mov edx, [esp + 4]   ; port
    xor eax, eax          ; zero EAX so upper bits are clean
    in ax, dx             ; 16-bit read
    ret

; void outw(uint16_t port, uint16_t value)
outw:
    mov edx, [esp + 4]   ; port
    mov eax, [esp + 8]   ; value (low 16 bits used)
    out dx, ax
    ret