[BITS 32]
global switch_to
global switch_paging
global kernel_task_trampoline
global user_task_trampoline
global fork_task_trampoline

extern SCHEDULE_IN_PROGRESS

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


kernel_task_trampoline:
    mov dword [SCHEDULE_IN_PROGRESS], 0
    sti                        
    ret                        

user_task_trampoline:
    mov dword [SCHEDULE_IN_PROGRESS], 0
    mov ax, 0x23              
    mov ds, ax
    mov es, ax
    iretd                     

fork_task_trampoline:
    mov dword [SCHEDULE_IN_PROGRESS], 0
    pop es
    pop ds
    popa
    iretd