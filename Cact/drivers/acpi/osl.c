#include "kernel.h"
#include "memory.h"
#include "sync.h"
#include "klib.h"
#include "irq.h"
#include "pci.h"
#include "task.h"
#include "proc/proc.h"
#include "acpi.h"
#include "cact_acpi.h"

#define ACPI_OSL_MAX_MAPPINGS  64

struct acpi_mapping {
    void*    virt;
    UINT32   phys;
    UINT32   size;
    int      used;
};

static struct acpi_mapping acpi_mappings[ACPI_OSL_MAX_MAPPINGS];
static spinlock_t          acpi_mappings_lock;
static UINT32              acpi_mapping_next_va = ACPI_TEMP_MAP_BASE;

static void* acpi_temp_map(UINT32 phys, UINT32 size)
{
    UINT32 offset    = phys & 0xFFF;
    UINT32 phys_page = phys & ~0xFFF;
    UINT32 pages     = ((offset + size + 0xFFF) >> 12);

    spinlock_acquire(&acpi_mappings_lock);
    UINT32 virt      = acpi_mapping_next_va;

    for (UINT32 i = 0; i < pages; i++) {
        vmm_map(get_current_pd(), virt + i * 4096, phys_page + i * 4096,
                PAGE_PRESENT | PAGE_RW | PAGE_PWT);
    }
    acpi_mapping_next_va += pages * 4096;

    struct acpi_mapping *m = NULL;
    for (int i = 0; i < ACPI_OSL_MAX_MAPPINGS; i++) {
        if (!acpi_mappings[i].used) { m = &acpi_mappings[i]; break; }
    }
    if (m) {
        m->virt = (void*)(UINT32)virt;
        m->phys = phys_page;
        m->size = pages * 4096;
        m->used = 1;
    }
    spinlock_release(&acpi_mappings_lock);
    return (void*)(UINT32)(virt + offset);
}

static void acpi_temp_unmap(void *virt, UINT32 size)
{
    UINT32 va      = (UINT32)(UINT32)virt & ~0xFFF;
    UINT32 pages   = (((UINT32)(UINT32)virt & 0xFFF) + size + 0xFFF) >> 12;
    UINT32 *pd     = get_current_pd();

    for (UINT32 i = 0; i < pages; i++) {
        UINT32 pde_idx = ((va + i * 4096) >> 22) & 0x3FF;
        UINT32 pte_idx = ((va + i * 4096) >> 12) & 0x3FF;
        UINT32 *pt = (UINT32*)(UINT32)(pd[pde_idx] & ~0xFFF);
        if (pt) pt[pte_idx] = 0;
    }
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");

    spinlock_acquire(&acpi_mappings_lock);
    for (int i = 0; i < ACPI_OSL_MAX_MAPPINGS; i++) {
        if (acpi_mappings[i].used && acpi_mappings[i].virt == (void*)(UINT32)va) {
            acpi_mappings[i].used = 0;
            break;
        }
    }
    spinlock_release(&acpi_mappings_lock);
}

static void udelay(UINT32 us)
{
    if (us == 0) return;
    UINT32 total = us * 1193;
    if (total == 0) total = 1;
    for (UINT32 i = 0; i < total; i++) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

ACPI_STATUS AcpiOsInitialize(void)
{
    spinlock_init(&acpi_mappings_lock);
    acpi_mapping_next_va = ACPI_TEMP_MAP_BASE;
    for (int i = 0; i < ACPI_OSL_MAX_MAPPINGS; i++)
        acpi_mappings[i].used = 0;
    return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void)
{
    return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
    UINT32 ebda_addr = 0;
    UINT16 *ebda_ptr = (UINT16*)(UINT32)0x40E;
    ebda_addr = *ebda_ptr;
    ebda_addr = (ebda_addr << 4) & 0xFFFFF;

    if (ebda_addr >= 0x80000 && ebda_addr <= 0x9FC00) {
        void *mem = AcpiOsMapMemory(ebda_addr, 4096);
        if (mem) {
            UINT8 *p = (UINT8*)mem;
            for (UINT32 off = 0; off <= 0x400 - sizeof(struct rsdp_descriptor); off += RSDP_SCAN_STEP) {
                if (p[off+0]=='R' && p[off+1]=='S' && p[off+2]=='D' && p[off+3]==' ' &&
                    p[off+4]=='P' && p[off+5]=='T' && p[off+6]=='R' && p[off+7]==' ') {
                    UINT8 sum = 0;
                    for (int i = 0; i < 20; i++) sum += p[off+i];
                    if (sum == 0) {
                        UINT32 pa = ebda_addr + off;
                        AcpiOsUnmapMemory(mem, 4096);
                        return pa;
                    }
                }
            }
            AcpiOsUnmapMemory(mem, 4096);
        }
    }

    for (UINT32 addr = RSDP_SCAN_START; addr < RSDP_SCAN_END; addr += 4096) {
        void *mem = AcpiOsMapMemory(addr, 4096);
        if (!mem) continue;
        UINT8 *p = (UINT8*)mem;
        for (UINT32 off = 0; off <= 4096 - sizeof(struct rsdp_descriptor); off += RSDP_SCAN_STEP) {
            if (p[off+0]=='R' && p[off+1]=='S' && p[off+2]=='D' && p[off+3]==' ' &&
                p[off+4]=='P' && p[off+5]=='T' && p[off+6]=='R' && p[off+7]==' ') {
                UINT8 sum = 0;
                for (int i = 0; i < 20; i++) sum += p[off+i];
                if (sum == 0) {
                    UINT32 pa = addr + off;
                    AcpiOsUnmapMemory(mem, 4096);
                    return pa;
                }
            }
        }
        AcpiOsUnmapMemory(mem, 4096);
    }
    return 0;
}

ACPI_STATUS AcpiOsPredefinedOverride(
    const ACPI_PREDEFINED_NAMES *InitVal,
    ACPI_STRING                 *NewVal)
{
    (void)InitVal;
    *NewVal = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(
    ACPI_TABLE_HEADER       *ExistingTable,
    ACPI_TABLE_HEADER       **NewTable)
{
    (void)ExistingTable;
    *NewTable = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(
    ACPI_TABLE_HEADER       *ExistingTable,
    ACPI_PHYSICAL_ADDRESS   *NewAddress,
    UINT32                  *NewTableLength)
{
    (void)ExistingTable;
    *NewAddress     = 0;
    *NewTableLength = 0;
    return AE_OK;
}

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length)
{
    return acpi_temp_map((UINT32)Where, (UINT32)Length);
}

void AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE Size)
{
    acpi_temp_unmap(LogicalAddress, (UINT32)Size);
}

ACPI_STATUS AcpiOsGetPhysicalAddress(
    void                    *LogicalAddress,
    ACPI_PHYSICAL_ADDRESS   *PhysicalAddress)
{
    if (!PhysicalAddress)
        return AE_BAD_PARAMETER;
    *PhysicalAddress = vmm_get_phys(get_current_pd(), (UINT32)(UINT32)LogicalAddress);
    return AE_OK;
}

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle)
{
    spinlock_t *lock = (spinlock_t*)kmalloc(sizeof(spinlock_t));
    if (!lock) return AE_NO_MEMORY;
    spinlock_init(lock);
    *OutHandle = lock;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
{
    kfree_heap(Handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
{
    spinlock_acquire((spinlock_t*)Handle);
    return 0;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
    (void)Flags;
    spinlock_release((spinlock_t*)Handle);
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
    kfree_heap(Handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(
    ACPI_SEMAPHORE          Handle,
    UINT32                  Units,
    UINT16                  Timeout)
{
    (void)Units;
    (void)Timeout;
    sema_down((semaphore_t*)Handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(
    ACPI_SEMAPHORE          Handle,
    UINT32                  Units)
{
    for (UINT32 i = 0; i < Units; i++)
        sema_up((semaphore_t*)Handle);
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
    kfree_heap(Handle);
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
    irq_register_handler(InterruptNumber, (void (*)(void))ServiceRoutine);
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
    udelay(Microseconds);
}

ACPI_STATUS AcpiOsReadPort(
    ACPI_IO_ADDRESS         Address,
    UINT32                  *Value,
    UINT32                  Width)
{
    switch (Width) {
    case 8:  *Value = port_byte_in((UINT16)Address); break;
    case 16: *Value = port_word_in((UINT16)Address); break;
    case 32: *Value = port_dword_in((UINT16)Address); break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(
    ACPI_IO_ADDRESS         Address,
    UINT32                  Value,
    UINT32                  Width)
{
    switch (Width) {
    case 8:  port_byte_out((UINT16)Address, (UINT8)Value); break;
    case 16: port_word_out((UINT16)Address, (UINT16)Value); break;
    case 32: port_dword_out((UINT16)Address, Value); break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsReadMemory(
    ACPI_PHYSICAL_ADDRESS   Address,
    UINT64                  *Value,
    UINT32                  Width)
{
    void *virt = acpi_temp_map((UINT32)Address, 4);
    if (!virt) return AE_BAD_ADDRESS;
    switch (Width) {
    case 8:  *Value = *(volatile UINT8*)virt;  break;
    case 16: *Value = *(volatile UINT16*)virt; break;
    case 32: *Value = *(volatile UINT32*)virt; break;
    case 64: *Value = *(volatile UINT64*)virt; break;
    default: acpi_temp_unmap(virt, 4); return AE_BAD_PARAMETER;
    }
    acpi_temp_unmap(virt, 4);
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(
    ACPI_PHYSICAL_ADDRESS   Address,
    UINT64                  Value,
    UINT32                  Width)
{
    void *virt = acpi_temp_map((UINT32)Address, 4);
    if (!virt) return AE_BAD_ADDRESS;
    switch (Width) {
    case 8:  *(volatile UINT8*)virt  = (UINT8)Value;  break;
    case 16: *(volatile UINT16*)virt = (UINT16)Value; break;
    case 32: *(volatile UINT32*)virt = (UINT32)Value; break;
    case 64: *(volatile UINT64*)virt = Value;          break;
    default: acpi_temp_unmap(virt, 4); return AE_BAD_PARAMETER;
    }
    acpi_temp_unmap(virt, 4);
    return AE_OK;
}

ACPI_STATUS AcpiOsReadPciConfiguration(
    ACPI_PCI_ID             *PciId,
    UINT32                  Reg,
    UINT64                  *Value,
    UINT32                  Width)
{
    UINT32 val = pci_read32(PciId->Bus, PciId->Device, PciId->Function,
                              (UINT8)(Reg & 0xFC));
    switch (Width) {
    case 8:  *Value = (UINT8)(val >> ((Reg & 3) * 8)); break;
    case 16: *Value = (UINT16)(val >> ((Reg & 2) * 8)); break;
    case 32: *Value = val; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(
    ACPI_PCI_ID             *PciId,
    UINT32                  Reg,
    UINT64                  Value,
    UINT32                  Width)
{
    UINT32 val = pci_read32(PciId->Bus, PciId->Device, PciId->Function,
                              (UINT8)(Reg & 0xFC));
    switch (Width) {
    case 8: {
        UINT32 shift = (Reg & 3) * 8;
        val &= ~(0xFFu << shift);
        val |= ((UINT8)Value) << shift;
        break;
    }
    case 16: {
        UINT32 shift = (Reg & 2) * 8;
        val &= ~(0xFFFFu << shift);
        val |= ((UINT16)Value) << shift;
        break;
    }
    case 32: val = (UINT32)Value; break;
    default: return AE_BAD_PARAMETER;
    }
    pci_write32(PciId->Bus, PciId->Device, PciId->Function, (UINT8)(Reg & 0xFC), val);
    return AE_OK;
}

void *AcpiOsAllocate(ACPI_SIZE Size)
{
    return kmalloc((UINT32)Size);
}

void AcpiOsFree(void *Memory)
{
    if (Memory) kfree_heap(Memory);
}

BOOLEAN AcpiOsReadable(void *Pointer, ACPI_SIZE Length)
{
    (void)Pointer;
    (void)Length;
    return TRUE;
}

BOOLEAN AcpiOsWritable(void *Pointer, ACPI_SIZE Length)
{
    (void)Pointer;
    (void)Length;
    return TRUE;
}

UINT64 AcpiOsGetTimer(void)
{
    UINT32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi << 32) | (UINT64)lo;
}

void AcpiOsPrintf(const char *Format, ...)
{
    (void)Format;
}

void AcpiOsVprintf(const char *Format, va_list Args)
{
    (void)Format;
    (void)Args;
}

void AcpiOsPuts(const char *String)
{
    if (String) {
        kprint((char*)"[ACPI] ");
        kprint((char*)String);
        kprint("\n");
    }
}

void AcpiOsRedirectOutput(void *Destination)
{
    (void)Destination;
}

ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info)
{
    (void)Function;
    (void)Info;
    return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep(
    UINT8                   SleepState,
    UINT32                  RegaValue,
    UINT32                  RegbValue)
{
    (void)SleepState;
    (void)RegaValue;
    (void)RegbValue;
    return AE_OK;
}

const UINT8 AcpiGbl_Ctypes[] = {
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    0x04,0x04,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
    0x08,0x08,0x08,0x20,0x20,0x20,0x20,0x20,
    0x20,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
    0x08,0x08,0x08,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
};

int tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

int toupper(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}
