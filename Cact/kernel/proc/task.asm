[BITS 32]
global switch_to
global switch_paging
global kernel_task_trampoline
global user_task_trampoline
global fork_task_trampoline

extern sched_park_prev


switch_paging:
    mov eax, [esp + 4]
    mov cr3, eax
    ret

switch_to:
    mov eax, [esp + 4]    ; prev task (`esp` is offset 0 of TaskStruct)
    mov edx, [esp + 8]    ; new_esp
    push ebp
    push edi
    push esi
    push ebx
    mov [eax], esp        ; commit the switch: save prev's stack pointer
    mov esp, edx          ; switch to the incoming stack
    push eax              ; arg: prev
    call sched_park_prev  ; now that prev is parked, make it runnable again
    add esp, 4
    pop ebx
    pop esi
    pop edi
    pop ebp
    ret


kernel_task_trampoline:
    sti
    ret

user_task_trampoline:
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    iretd

fork_task_trampoline:
    pop es
    pop ds
    popa
    iretd