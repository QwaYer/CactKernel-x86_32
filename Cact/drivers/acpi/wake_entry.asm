; acpi_do_suspend — save the resume context, enter S-state, come back here.
;
; The platform resets the CPU on an S3 wake, so the resume path
; (acpi_ctx_restore in power.c) restores CR3/ESP/EBP/EBX/ESI/EDI and jumps to
; g_resume.eip, which this routine points at acpi_resume_point.  A plain symbol
; is used instead of GCC's labels-as-values extension, which clang -O2
; miscompiled to the constant 1 in this file.
;
; int acpi_do_suspend(uint32_t state)
;   returns 0 when the platform woke us, -1 when it did not sleep at all.

[bits 32]

extern g_resume                    ; cr3, esp, ebp, ebx, esi, edi, eip
extern AcpiEnterSleepState

global acpi_do_suspend

acpi_do_suspend:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    push edi

    mov  eax, cr3
    mov  [g_resume + 0], eax       ; cr3
    mov  [g_resume + 4], esp       ; esp (points at the saved edi)
    mov  [g_resume + 8], ebp
    mov  [g_resume + 12], ebx
    mov  [g_resume + 16], esi
    mov  [g_resume + 20], edi
    mov  eax, acpi_resume_point
    mov  [g_resume + 24], eax      ; eip

    cli
    push dword [ebp + 8]           ; state argument
    call AcpiEnterSleepState
    add  esp, 4

    ; Only reached when the platform did not reset.
    sti
    mov  eax, -1
    jmp  acpi_do_suspend_done

acpi_resume_point:                 ; entered by acpi_ctx_restore
    sti
    xor  eax, eax

acpi_do_suspend_done:
    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret
