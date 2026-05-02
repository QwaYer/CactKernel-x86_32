[bits 32]

; I/O port access primitives — cdecl calling convention
; Each function takes port number as first argument, value (for out) as second.
; Return value in EAX for in functions.

global port_long_out
global port_long_in
global port_byte_out
global port_byte_in
global port_word_in
global port_word_out 

; void port_long_out(uint16_t port, uint32_t value)
port_long_out:
    mov edx, [esp + 4]   ; port
    mov eax, [esp + 8]   ; value
    out dx, eax
    ret

; uint32_t port_long_in(uint16_t port)
port_long_in:
    mov edx, [esp + 4]   ; port
    in eax, dx            ; 32-bit read — EAX fully overwritten
    ret

; void port_byte_out(uint16_t port, uint8_t value)
port_byte_out:
    mov edx, [esp + 4]   ; port
    mov al, [esp + 8]    ; value (low byte only)
    out dx, al
    ret

; uint8_t port_byte_in(uint16_t port)
; NOTE: upper 24 bits of EAX are not zeroed — caller must mask if needed
port_byte_in:
    mov edx, [esp + 4]   ; port
    in al, dx             ; 8-bit read — AH and upper bits untouched
    ret

; uint16_t port_word_in(uint16_t port)
port_word_in:
    mov edx, [esp + 4]   ; port
    xor eax, eax          ; zero EAX so upper bits are clean
    in ax, dx             ; 16-bit read
    ret

; void port_word_out(uint16_t port, uint16_t value)
port_word_out:
    mov edx, [esp + 4]   ; port
    mov eax, [esp + 8]   ; value (low 16 bits used)
    out dx, ax
    ret