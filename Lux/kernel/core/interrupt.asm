[bits 32]

global keyboard_isr
global virtio_net_isr
global timer_isr
global syscall_isr
global isr_common_stub
global isr0
global isr1
global isr2
global isr3
global isr4
global isr5
global isr6
global isr7
global isr8
global isr9
global isr10
global isr11
global isr12
global isr13
global isr14
global isr15
global isr16
global isr17
global isr18
global isr19
global isr20
global isr21
global isr22
global isr23
global isr24
global isr25
global isr26
global isr27
global isr28
global isr29
global isr30
global isr31
global nvme_isr
global mouse_isr
global xhci_isr
global usb_isr
global spurious_irq7
global spurious_irq15

extern ps2_keyboard_handler
extern virtio_net_irq_handler
extern exception_handler
extern syscall_handler
extern nvme_irq_handler
extern ps2_mouse_handler
extern current_task
extern schedule
extern page_fault_handler
extern xhci_irq_handler
extern tss_entry
extern page_directory

global timer_ticks_get

section .data
timer_ticks dd 0

section .text

timer_ticks_get:
    mov eax, [timer_ticks]
    ret

isr0:
    push dword 0
    push dword 0
    jmp isr_common_stub

isr1:
    push dword 0
    push dword 1
    jmp isr_common_stub

isr2:
    push dword 0
    push dword 2
    jmp isr_common_stub

isr3:
    push dword 0
    push dword 3
    jmp isr_common_stub

isr4:
    push dword 0
    push dword 4
    jmp isr_common_stub

isr5:
    push dword 0
    push dword 5
    jmp isr_common_stub

isr6:
    push dword 0
    push dword 6
    jmp isr_common_stub

isr7:
    push dword 0
    push dword 7
    jmp isr_common_stub

isr8:
    push dword 8
    jmp isr_common_stub

isr9:
    push dword 0
    push dword 9
    jmp isr_common_stub

isr10:
    push dword 10
    jmp isr_common_stub

isr11:
    push dword 11
    jmp isr_common_stub

isr12:
    push dword 12
    jmp isr_common_stub

isr13:
    push dword 13
    jmp isr_common_stub

isr14:
    push dword 14
    push eax
    mov  eax, [esp+8]
    mov  [esp+8], dword 14
    mov  [esp+4], eax
    pop  eax
    jmp  isr_pf_stub

isr15:
    push dword 0
    push dword 15
    jmp isr_common_stub

isr16:
    push dword 0
    push dword 16
    jmp isr_common_stub

isr17:
    push dword 17
    jmp isr_common_stub

isr18:
    push dword 0
    push dword 18
    jmp isr_common_stub

isr19:
    push dword 0
    push dword 19
    jmp isr_common_stub

isr20:
    push dword 0
    push dword 20
    jmp isr_common_stub

isr21:
    push dword 21
    jmp isr_common_stub

isr22:
    push dword 0
    push dword 22
    jmp isr_common_stub

isr23:
    push dword 0
    push dword 23
    jmp isr_common_stub

isr24:
    push dword 0
    push dword 24
    jmp isr_common_stub

isr25:
    push dword 0
    push dword 25
    jmp isr_common_stub

isr26:
    push dword 0
    push dword 26
    jmp isr_common_stub

isr27:
    push dword 0
    push dword 27
    jmp isr_common_stub

isr28:
    push dword 0
    push dword 28
    jmp isr_common_stub

isr29:
    push dword 29
    jmp isr_common_stub

isr30:
    push dword 30
    jmp isr_common_stub

isr31:
    push dword 0
    push dword 31
    jmp isr_common_stub

isr_pf_stub:
    pusha
    push ds
    push es

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    push esp
    call page_fault_handler
    add esp, 4

    pop es
    pop ds
    popa
    add esp, 8
    iretd

isr_common_stub:
    pusha
    push ds
    push es

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    push esp
    call exception_handler
    add esp, 4

    pop es
    pop ds
    popa
    add esp, 8
    iretd

;
; timer_isr — preemptive context switch
;
; Stack on entry (pushed by CPU + our code):
;   [ss, useresp]  (only on ring change 3→0)
;   eflags, cs, eip
;   pusha (8 regs)
;   ds, es
;   ← esp saved here into current_task->esp
;
; After schedule() picks a new current_task, we:
;   1) restore esp from new task
;   2) switch cr3 if task has its own page_directory
;   3) update tss.esp0 for ring3→ring0 transitions
;   4) send EOI
;   5) pop es, ds, popa, iretd
;
timer_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    inc dword [timer_ticks]

    ; save current esp into current_task->esp (offset 0)
    mov eax, [current_task]
    test eax, eax
    jz .skip_save
    mov [eax], esp

.skip_save:
    call schedule

    ; load new task's esp
    mov eax, [current_task]
    test eax, eax
    jz .do_eoi
    mov esp, [eax]

    ; switch cr3: task->page_directory is at offset 28
    ; struct offsets: esp(0) pid(4) state(8) is_kernel(12) stack_base(16)
    ;                 ustack_phys(20) ustack_virt(24) page_directory(28)
    mov ebx, [eax + 28]
    test ebx, ebx
    jz .use_kernel_pd
    mov cr3, ebx
    jmp .update_tss

.use_kernel_pd:
    mov ebx, page_directory
    mov cr3, ebx

.update_tss:
    ; tss_entry.esp0 = task->stack_base + 4096
    ; stack_base is at offset 16
    mov ecx, [eax + 16]
    test ecx, ecx
    jz .do_eoi
    add ecx, 4096
    mov [tss_entry + 4], ecx   ; tss_entry.esp0 is at offset 4

.do_eoi:
    mov al, 0x20
    out 0x20, al

    pop es
    pop ds
    popa
    iretd

keyboard_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call ps2_keyboard_handler
    mov al, 0x20
    out 0x20, al
    pop es
    pop ds
    popa
    iretd

mouse_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call ps2_mouse_handler
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop es
    pop ds
    popa
    iretd

nvme_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call nvme_irq_handler
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop es
    pop ds
    popa
    iretd

virtio_net_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call virtio_net_irq_handler
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop es
    pop ds
    popa
    iretd

usb_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call xhci_irq_handler
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop es
    pop ds
    popa
    iretd

xhci_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call xhci_irq_handler
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop es
    pop ds
    popa
    iretd

; Syscall (INT 0x80)
; Same context-switch pattern as timer_isr
syscall_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    push esp
    call syscall_handler
    add esp, 4

    ; save & switch
    mov eax, [current_task]
    test eax, eax
    jz .sc_no_switch

    mov [eax], esp
    call schedule
    mov eax, [current_task]
    mov esp, [eax]

    ; switch cr3
    mov ebx, [eax + 28]
    test ebx, ebx
    jz .sc_kernel_pd
    mov cr3, ebx
    jmp .sc_update_tss

.sc_kernel_pd:
    mov ebx, page_directory
    mov cr3, ebx

.sc_update_tss:
    mov ecx, [eax + 16]
    test ecx, ecx
    jz .sc_no_switch
    add ecx, 4096
    mov [tss_entry + 4], ecx

.sc_no_switch:
    pop es
    pop ds
    popa
    iretd

; Spurious IRQ7 (master PIC, vector 0x27)
spurious_irq7:
    push eax
    mov al, 0x0B
    out 0x20, al
    in  al, 0x20
    test al, 0x80
    jz .spur7
    mov al, 0x20
    out 0x20, al
.spur7:
    pop eax
    iretd

; Spurious IRQ15 (slave PIC, vector 0x2F)
spurious_irq15:
    push eax
    mov al, 0x0B
    out 0xA0, al
    in  al, 0xA0
    test al, 0x80
    jz .spur15
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop eax
    iretd
.spur15:
    mov al, 0x20
    out 0x20, al
    pop eax
    iretd