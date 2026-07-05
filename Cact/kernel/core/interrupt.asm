[bits 32]

global timer_isr
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
extern g_syscall_use_sysexit

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
;
; User convention (set by CactLib stubs):
;   EAX = syscall number
;   EBX = arg1
;   ESI = arg2          (stub remaps ecx→esi because CPU steals ECX)
;   EDI = arg3          (stub remaps edx→edi because CPU steals EDX)
;   ECX = return ESP    (CPU-saved — user stack pointer)
;   EDX = return EIP    (CPU-saved — instruction after sysenter)
;
; We remap ESI→frame.ecx and EDI→frame.edx so the existing C dispatcher
; and all 95 sys_* handlers see the exact same (ebx,ecx,edx) ABI as the
; old int 0x80 path — no C changes required.
;
; Return is either IRET (atomic EFLAGS restore) or SYSEXIT (faster, manual
; EFLAGS restore), selected at runtime by g_syscall_use_sysexit.
;
; GDT is already SYSEXIT-compatible: user CS=0x18=SYSENTER_CS+0x10,
; user DS=0x20=SYSENTER_CS+0x18, so no GDT reorganisation is needed.
; ---------------------------------------------------------------------------
global sysenter_entry
sysenter_entry:
    push edx                         ; save return_eip  → frame[56]
    push ecx                         ; save user_esp    → frame[52]
    push eax                         ; save syscall_num → frame[48]
    sub esp, 48                      ; 60-byte syscall_frame total

    ; --- pusha area (offsets 0–36) ---
    ; frame.es/ds = user data (0x23), NOT kernel data — fork copies these into
    ; the child's context_frame and fork_task_trampoline restores them via
    ; pop es/pop ds.  0x10 here would give the child a DPL-0 segment → #GP in ring3.
    mov [esp + 0],  dword 0x23       ; es  (user data — saved for fork/iret)
    mov [esp + 4],  dword 0x23       ; ds  (user data — saved for fork/iret)
    mov [esp + 8],  edi              ; edi  = arg3
    mov [esp + 12], esi              ; esi  = arg2
    mov [esp + 16], ebp              ; ebp
    mov [esp + 20], dword 0          ; esp_dummy
    mov [esp + 24], ebx              ; ebx  = arg1
    mov [esp + 28], edi              ; edx  ← arg3 (remapped from EDI)
    mov [esp + 32], esi              ; ecx  ← arg2 (remapped from ESI)

    ; --- copy saved values into frame slots ---
    mov eax, [esp + 48]              ; syscall number
    mov [esp + 36], eax              ; eax
    mov ecx, [esp + 52]              ; user_esp
    mov edx, [esp + 56]              ; return_eip
    mov [esp + 40], edx              ; eip
    mov [esp + 52], ecx              ; useresp

    ; --- iretd frame ---
    mov [esp + 44], dword 0x1B       ; cs  (user code | RPL3)
    mov [esp + 56], dword 0x23       ; ss  (user data | RPL3)
    pushfd
    pop eax
    or eax, 0x200                    ; IF=1 — user code runs with interrupts on
    mov [esp + 48], eax              ; eflags

    ; kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; dispatch
    push esp
    call syscall_handler
    add esp, 4

    ; ===== return path selection =====
    cmp byte [g_syscall_use_sysexit], 0
    jne .do_sysexit

    ; --- IRET return (pop es/ds/popa restores user DS/ES from frame) ---
.do_iret:
    pop es                           ; ES = frame.es = 0x23
    pop ds                           ; DS = frame.ds = 0x23
    popa                             ; edi,esi,ebp,skip,ebx,edx,ecx,eax(incl. ret val)
    iretd                            ; pops eip,cs,eflags,useresp,ss

    ; --- SYSEXIT return ---
.do_sysexit:
    push dword [esp + 48]            ; restore EFLAGS (IF etc.) before sysexit
    popfd

    mov esi, [esp + 12]
    mov edi, [esp + 8]
    mov ebx, [esp + 24]
    mov ebp, [esp + 16]

    mov eax, 0x23                    ; user data segment (sysexit sets SS, not DS/ES)
    mov ds, ax
    mov es, ax

    mov ecx, [esp + 52]              ; user_esp    → ECX (load before edx)
    mov edx, [esp + 40]              ; return EIP  → EDX
    mov eax, [esp + 36]              ; return value → EAX (last)
    sysexit

section .rodata
global msix_stub_table
msix_stub_table:
%assign msix_j 0x30
%rep 192
    dd msix_stub_%+msix_j
%assign msix_j msix_j+1
%endrep