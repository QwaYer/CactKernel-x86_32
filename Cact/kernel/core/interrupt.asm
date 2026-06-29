[bits 32]

global timer_isr
global syscall_isr
global pci_isr
global acpi_sci_isr
global spurious_apic_isr
global isr_common_stub
global isr0
global isr1
global isr2
global isr3
global isr4
global isr5
global isr6
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
global xhci_isr

extern exception_handler
extern syscall_handler
extern acpi_sci_callback
extern current_task
extern schedule
extern on_timer_tick       
extern acpi_pm_timer_tick
extern timer_eoi
extern irq_apic_eoi
extern page_fault_handler
extern xhci_irq_handler
extern tss_entry
extern page_directory

section .text

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

extern task_trampoline_addr   

timer_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    call acpi_pm_timer_tick

    call timer_eoi

    call on_timer_tick

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
    call irq_apic_eoi
    pop es
    pop ds
    popa
    iretd

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

.sc_no_switch:
    pop es
    pop ds
    popa
    iretd

acpi_sci_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call acpi_sci_callback
    call irq_apic_eoi
    pop es
    pop ds
    popa
    iretd

; ---------------------------------------------------------------------------
; Generic PCI INTx ISR (vectors 0xF0+). Just EOIs.
; ---------------------------------------------------------------------------
global pci_isr
pci_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call irq_apic_eoi
    pop es
    pop ds
    popa
    iretd

; ---------------------------------------------------------------------------
; MSI-X stubs (vectors 0x30–0xEF). Each pushes the vector and dispatches.
; ---------------------------------------------------------------------------
extern msix_dispatch

%macro msix_entry 1
global msix_stub_%1
msix_stub_%1:
    push dword %1
    jmp msix_common_dispatch
%endmacro

%assign msix_vec 0x30
%rep 192
msix_entry msix_vec
%assign msix_vec msix_vec+1
%endrep

global msix_common_dispatch
msix_common_dispatch:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov esi, [esp + 40]
    push esi
    call msix_dispatch
    add esp, 4
    call irq_apic_eoi
    pop es
    pop ds
    popa
    add esp, 4
    iretd

; ---------------------------------------------------------------------------
; APIC spurious interrupt handler — must be valid but does nothing (APIC
; doesn't expect EOI for spurious vectors).
; ---------------------------------------------------------------------------
global spurious_apic_isr
spurious_apic_isr:
    iretd

; ---------------------------------------------------------------------------
; Fast syscall entry point (SYSENTER / IA32_SYSENTER_EIP).
; On entry:
;   EAX = system call number
;   EBX = arg1
;   ECX = user ESP         (saved here by CPU, original arg2 lost)
;   EDX = return EIP       (saved here by CPU, original arg3 lost)
;   ESI = arg4
;   EDI = arg5
;
; Stack must use iretd for now (sysexit needs GDT reorganisation:
; user code at IA32_SYSENTER_CS+16 = 0x18, user data at +24 = 0x20).
; ---------------------------------------------------------------------------
global sysenter_entry
sysenter_entry:
    push edx                          ; [orig_esp - 4] = return EIP
    push ecx                          ; [orig_esp - 8] = user ESP
    sub esp, 52                       ; ESP = orig_esp - 60

    mov ecx, [esp + 52]               ; saved user ESP  (2nd push)
    mov edx, [esp + 56]               ; saved return EIP (1st push)

    mov [esp + 56], dword 0x23        ; ss
    mov [esp + 52], ecx               ; useresp
    pushfd
    pop eax
    mov [esp + 48], eax               ; eflags
    mov [esp + 44], dword 0x1B        ; cs (user code)
    mov [esp + 40], edx               ; eip
    mov [esp + 36], eax               ; syscall number → eax in frame
    mov [esp + 32], ecx               ; ecx in frame (= user ESP, arg2 lost)
    mov [esp + 28], edx               ; edx in frame (= ret EIP,  arg3 lost)
    mov [esp + 24], ebx               ; arg1
    mov [esp + 20], ecx               ; esp_dummy
    mov [esp + 16], ebp               ; ebp
    mov [esp + 12], esi               ; arg4
    mov [esp +  8], edi               ; arg5
    mov [esp +  4], dword 0x10        ; ds (kernel data)
    mov [esp +  0], dword 0x10        ; es (kernel data)

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    push esp
    call syscall_handler
    add esp, 4

    mov eax, [esp + 36]               ; return value
    mov ecx, [esp + 52]               ; user ESP
    mov edx, [esp + 40]               ; return EIP
    mov esi, [esp + 12]
    mov edi, [esp +  8]
    mov ebx, [esp + 24]
    mov ebp, [esp + 16]

    add esp, 40
    iretd

section .rodata
global msix_stub_table
msix_stub_table:
%assign msix_j 0x30
%rep 192
    dd msix_stub_%+msix_j
%assign msix_j msix_j+1
%endrep