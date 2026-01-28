[BITS 32]
global switch_to
global switch_paging

switch_paging:
    mov eax, [esp + 4]
    mov cr3, eax
    ret

switch_to:
    mov eax, [esp + 4]    ; &old_esp
    mov edx, [esp + 8]    ; new_esp
    push ebp
    push edi
    push esi
    push ebx
    mov [eax], esp
    mov esp, edx
    pop ebx
    pop esi
    pop edi
    pop ebp
    ret