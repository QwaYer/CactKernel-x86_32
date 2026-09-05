[bits 32]

global sysenter_entry
global syscall_entry

extern syscall_handler
extern tss_entry
extern g_syscall_use_sysexit

section .text

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
;
; Return is either IRET (atomic EFLAGS restore) or SYSEXIT (faster, manual
; EFLAGS restore), selected at runtime by g_syscall_use_sysexit.
;
; GDT is already SYSEXIT-compatible: user CS=0x18=SYSENTER_CS+0x10,
; user DS=0x20=SYSENTER_CS+0x18, so no GDT reorganisation is needed.
; ---------------------------------------------------------------------------
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

; ---------------------------------------------------------------------------
; AMD fast-syscall entry point (SYSCALL / EFER.SCE + STAR).
;
; SYSCALL is an AMD-defined fast syscall: unlike SYSENTER it saves the return
; EIP into ECX automatically and does NOT switch stacks.  In 32-bit legacy
; mode it also stores EFLAGS into R11 (inaccessible from 32-bit code, but the
; CPU restores it automatically on SYSRET) and masks EFLAGS with FMASK — we
; set FMASK=0x200 so IF stays cleared while we leave the user stack.
;
; User convention (CactLib syscall stub, SYSCALL variant):
;   EAX = syscall number
;   EBX = arg1
;   ESI = arg2     (stub uses the same ESI/EDI arg registers as SYSENTER)
;   EDI = arg3
;   ECX = return EIP (CPU-saved — the stub emits a bare `syscall`)
;   ESP = user ESP  (CPU leaves it untouched — grabbed here before switching)
;
; We grab the user ESP straight from ESP, switch to the per-task kernel stack
; kept current in tss_entry.esp0 by the scheduler, then build the exact same
; 60-byte syscall_frame the C dispatcher already understands.  Return is via
; IRET only: SYSRET's selector math (+0/+8 split for CS/SS) is incompatible
; with this GDT's user-code-then-user-data ordering, and IRET is fully general
; and already proven correct by the SYSENTER IRET path.
;
; GDT: 0x08 kcode, 0x10 kdata, 0x18 ucode, 0x20 udata.
; ---------------------------------------------------------------------------
syscall_entry:
    ; IF is already cleared by FMASK — safe to touch the user stack.
    mov edx, esp              ; EDX = user ESP (SYSCALL doesn't save it)
    mov esp, [tss_entry + 4]  ; switch to kernel stack (tss_entry.esp0)
    sti                       ; on the kernel stack now — interrupts may run

    sub esp, 60               ; allocate the 60-byte syscall_frame

    ; ECX = return EIP, EDX = user ESP — capture before any register reuse.
    mov [esp + 40], ecx       ; frame.eip   = return EIP
    mov [esp + 52], edx       ; frame.useresp = user ESP

    ; --- pusha area (offsets 0–36) ---
    mov [esp + 0],  dword 0x23   ; es  (user data — saved for fork/iret)
    mov [esp + 4],  dword 0x23   ; ds  (user data)
    mov [esp + 8],  edi          ; edi  = arg3
    mov [esp + 12], esi          ; esi  = arg2
    mov [esp + 16], ebp          ; ebp
    mov [esp + 20], dword 0      ; esp_dummy
    mov [esp + 24], ebx          ; ebx  = arg1
    mov [esp + 28], edi          ; edx  ← arg3 (remapped from EDI)
    mov [esp + 32], esi          ; ecx  ← arg2 (remapped from ESI)
    mov [esp + 36], eax          ; eax  = syscall number

    ; --- iretd frame ---
    mov [esp + 44], dword 0x1B   ; cs  (user code | RPL3)
    mov [esp + 48], dword 0x202  ; eflags (IF=1, reserved bit1=1)
    mov [esp + 56], dword 0x23   ; ss  (user data | RPL3)

    ; kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; dispatch
    push esp
    call syscall_handler
    add esp, 4

    ; ===== return path: IRET (pop es/ds/popa restores user DS/ES from frame) =====
    pop es                           ; ES = frame.es = 0x23
    pop ds                           ; DS = frame.ds = 0x23
    popa                             ; edi,esi,ebp,skip,ebx,edx,ecx,eax(ret val)
    iretd                            ; pops eip,cs,eflags,useresp,ss
