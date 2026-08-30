#include "kernel.h"
#include "memory.h"
#include "sync.h"
#include "klib.h"
#include "pci.h"
#include "task.h"
#include "proc/proc.h"
#include "acpi.h"
#include "cact_acpi.h"
#include "idt.h"

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle)
{
    spinlock_t *lock = (spinlock_t*)kmalloc(sizeof(spinlock_t));
    if (!lock) return AE_NO_MEMORY;
    spin_lock_init(lock);
    *OutHandle = lock;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
{
    kfree(Handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
{
    spin_lock((spinlock_t*)Handle);
    return 0;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
    (void)Flags;
    spin_unlock((spinlock_t*)Handle);
}

ACPI_STATUS AcpiOsCreateSemaphore(
    UINT32                  MaxUnits,
    UINT32                  InitialUnits,
    ACPI_SEMAPHORE          *OutHandle)
{
    (void)MaxUnits;
    semaphore_t *s = (semaphore_t*)kmalloc(sizeof(semaphore_t));
    if (!s) return AE_NO_MEMORY;
    sema_init(s, (int)InitialUnits);
    *OutHandle = s;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle)
{
    kfree(Handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(
    ACPI_SEMAPHORE          Handle,
    UINT32                  Units,
    UINT16                  Timeout)
{
    (void)Units;
    (void)Timeout;
    down((semaphore_t*)Handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(
    ACPI_SEMAPHORE          Handle,
    UINT32                  Units)
{
    for (UINT32 i = 0; i < Units; i++)
        up((semaphore_t*)Handle);
    return AE_OK;
}

#if (ACPI_MUTEX_TYPE != ACPI_BINARY_SEMAPHORE)

ACPI_STATUS AcpiOsCreateMutex(
    ACPI_MUTEX              *OutHandle)
{
    mutex_t *m = (mutex_t*)kmalloc(sizeof(mutex_t));
    if (!m) return AE_NO_MEMORY;
    mutex_init(m);
    *OutHandle = m;
    return AE_OK;
}

void AcpiOsDeleteMutex(ACPI_MUTEX Handle)
{
    kfree(Handle);
}

ACPI_STATUS AcpiOsAcquireMutex(
    ACPI_MUTEX              Handle,
    UINT16                  Timeout)
{
    (void)Timeout;
    mutex_lock((mutex_t*)Handle);
    return AE_OK;
}

void AcpiOsReleaseMutex(ACPI_MUTEX Handle)
{
    mutex_unlock((mutex_t*)Handle);
}

#endif

ACPI_STATUS AcpiOsInstallInterruptHandler(
    UINT32                  InterruptNumber,
    ACPI_OSD_HANDLER        ServiceRoutine,
    void                    *Context)
{
    (void)Context;
    extern void (*acpi_sci_callback)(void);
    extern void acpi_sci_isr();
    acpi_sci_callback = (void (*)(void))ServiceRoutine;
    set_idt_gate(0x20 + InterruptNumber, (uint32_t)acpi_sci_isr);
    return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(
    UINT32                  InterruptNumber,
    ACPI_OSD_HANDLER        ServiceRoutine)
{
    (void)InterruptNumber;
    (void)ServiceRoutine;
    return AE_OK;
}

ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    return 1;
}

ACPI_STATUS AcpiOsExecute(
    ACPI_EXECUTE_TYPE       Type,
    ACPI_OSD_EXEC_CALLBACK  Function,
    void                    *Context)
{
    (void)Type;
    (void)Function;
    (void)Context;
    return AE_OK;
}

void AcpiOsWaitEventsComplete(void)
{
}

void AcpiOsSleep(UINT64 Milliseconds)
{
    UINT32 ticks = (UINT32)(Milliseconds / 10);
    if (ticks == 0) ticks = 1;
    sched_sleep_ticks(ticks);
}

void AcpiOsStall(UINT32 Microseconds)
{
    osl_udelay(Microseconds);
}
