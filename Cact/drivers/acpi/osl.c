#include "kernel.h"
#include "memory.h"
#include "sync.h"
#include "klib.h"
#include "pci.h"
#include "task.h"
#include "process/proc.h"
#include "acpi.h"
#include "cact_acpi.h"
#include "idt.h"

#define ACPI_OSL_MAX_MAPPINGS  64

/*
 * Stable, reference-counted temporary mapping of ACPI physical memory.
 *
 * The same physical page is always mapped at the same virtual address, so a
 * persistent ACPI table pointer (Table->Pointer) stays valid across the
 * validate/parse/re-parse cycles instead of being re-mapped to a fresh VA each
 * time.  The mapping window is bounded by the number of distinct physical pages
 * (not the number of map/unmap calls), so it cannot creep into MMIO/framebuffer
 * space.  A mapping is torn down only when its last reference is released;
 * a freed slot is reused for the same physical page first to keep its VA stable.
 */
struct acpi_mapping {
    void*    virt;    /* page-aligned base VA for this physical page */
    UINT32   phys;    /* page-aligned physical address */
    UINT32   pages;   /* number of 4K pages currently mapped */
    UINT32   refs;    /* outstanding references */
};

static struct acpi_mapping acpi_mappings[ACPI_OSL_MAX_MAPPINGS];
static spinlock_t          acpi_mappings_lock;
static UINT32              acpi_mapping_next_va = ACPI_TEMP_MAP_BASE;

void* acpi_temp_map(UINT32 phys, UINT32 size)
{
    UINT32 offset    = phys & 0xFFF;
    UINT32 phys_page = phys & ~0xFFF;
    UINT32 pages     = ((offset + size + 0xFFF) >> 12);

    spin_lock(&acpi_mappings_lock);

    /* Reuse an existing mapping of the SAME physical page (keeps the VA stable). */
    struct acpi_mapping *m = NULL;
    for (int i = 0; i < ACPI_OSL_MAX_MAPPINGS; i++) {
        if (acpi_mappings[i].phys == phys_page) { m = &acpi_mappings[i]; break; }
    }

    if (m) {
        if (pages > m->pages) {
            for (UINT32 i = m->pages; i < pages; i++) {
                vmm_map(get_current_pd(), (UINT32)m->virt + i * 4096,
                        phys_page + i * 4096, PAGE_PRESENT | PAGE_RW | PAGE_PWT);
            }
            m->pages = pages;
        }
        m->refs++;
        spin_unlock(&acpi_mappings_lock);
        return (void*)(UINT32)((UINT32)m->virt + offset);
    }

    /* Otherwise claim a free slot. */
    for (int i = 0; i < ACPI_OSL_MAX_MAPPINGS; i++) {
        if (acpi_mappings[i].refs == 0) { m = &acpi_mappings[i]; break; }
    }

    UINT32 virt = acpi_mapping_next_va;
    for (UINT32 i = 0; i < pages; i++) {
        vmm_map(get_current_pd(), virt + i * 4096, phys_page + i * 4096,
                PAGE_PRESENT | PAGE_RW | PAGE_PWT);
    }
    acpi_mapping_next_va += pages * 4096;

    if (m) {
        m->virt  = (void*)(UINT32)virt;
        m->phys  = phys_page;
        m->pages = pages;
        m->refs  = 1;
    }
    spin_unlock(&acpi_mappings_lock);
    return (void*)(UINT32)(virt + offset);
}

void acpi_temp_unmap(void *virt, UINT32 size)
{
    UINT32 va      = (UINT32)(UINT32)virt & ~0xFFF;
    UINT32 *pd     = get_current_pd();

    spin_lock(&acpi_mappings_lock);

    struct acpi_mapping *m = NULL;
    for (int i = 0; i < ACPI_OSL_MAX_MAPPINGS; i++) {
        if ((UINT32)(UINT32)acpi_mappings[i].virt == va && acpi_mappings[i].refs) {
            m = &acpi_mappings[i];
            break;
        }
    }

    if (m && m->refs > 0) {
        m->refs--;
        if (m->refs == 0) {
            /* Last reference: tear the mapping down, but keep phys so the VA
             * is reused for the same physical page (stable header). */
            for (UINT32 i = 0; i < m->pages; i++) {
                UINT32 pde_idx = ((va + i * 4096) >> 22) & 0x3FF;
                UINT32 pte_idx = ((va + i * 4096) >> 12) & 0x3FF;
                UINT32 *pt = (UINT32*)(UINT32)(pd[pde_idx] & ~0xFFF);
                if (pt) pt[pte_idx] = 0;
            }
            asm volatile("invlpg (%0)" :: "r"(va) : "memory");
            m->pages = 0;
            m->refs  = 0;
        }
    }

    spin_unlock(&acpi_mappings_lock);
}

void osl_udelay(UINT32 us)
{
    if (us == 0) return;
    while (us > 0) {
        UINT32 chunk = (us > 3598975u) ? 3598975u : us;
        UINT32 total = chunk * 1193;
        for (UINT32 i = 0; i < total; i++) {
            __asm__ __volatile__("pause" ::: "memory");
        }
        us -= chunk;
    }
}

ACPI_STATUS AcpiOsInitialize(void)
{
    spin_lock_init(&acpi_mappings_lock);
    acpi_mapping_next_va = ACPI_TEMP_MAP_BASE;
    for (int i = 0; i < ACPI_OSL_MAX_MAPPINGS; i++) {
        acpi_mappings[i].virt  = NULL;
        acpi_mappings[i].phys  = 0;
        acpi_mappings[i].pages = 0;
        acpi_mappings[i].refs  = 0;
    }
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

void *AcpiOsAllocate(ACPI_SIZE Size)
{
    return kmalloc((UINT32)Size);
}

void AcpiOsFree(void *Memory)
{
    if (Memory) kfree(Memory);
}

struct acpi_osl_cache {
    UINT16  object_size;
    UINT16  max_depth;
};

ACPI_STATUS AcpiOsCreateCache(
    char                    *CacheName,
    UINT16                  ObjectSize,
    UINT16                  MaxDepth,
    ACPI_CACHE_T            **ReturnCache)
{
    (void)CacheName;
    if (!ReturnCache) return AE_NO_MEMORY;
    struct acpi_osl_cache    *c = (struct acpi_osl_cache *)kmalloc(sizeof(*c));
    if (!c) return AE_NO_MEMORY;
    c->object_size = ObjectSize;
    c->max_depth   = MaxDepth;
    *ReturnCache   = (ACPI_CACHE_T *)c;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteCache(ACPI_CACHE_T *Cache)
{
    if (Cache) kfree(Cache);
    return AE_OK;
}

ACPI_STATUS AcpiOsPurgeCache(ACPI_CACHE_T *Cache)
{
    (void)Cache;
    return AE_OK;
}

void *AcpiOsAcquireObject(ACPI_CACHE_T *Cache)
{
    struct acpi_osl_cache *c = (struct acpi_osl_cache *)Cache;
    void *obj = kmalloc(c ? c->object_size : 0);
    if (obj && c)
        memset(obj, 0, c->object_size);
    return obj;
}

ACPI_STATUS AcpiOsReleaseObject(ACPI_CACHE_T *Cache, void *Object)
{
    (void)Cache;
    if (Object) kfree(Object);
    return AE_OK;
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
    va_list Args;
    va_start(Args, Format);
    AcpiOsVprintf(Format, Args);
    va_end(Args);
}

void AcpiOsVprintf(const char *Format, va_list Args)
{
    char buf[256];
    extern int vsnprintf(char *String, unsigned int Size, const char *Format, va_list Args);
    vsnprintf(buf, sizeof(buf), Format, Args);
    printk(buf);
}

void AcpiOsPuts(const char *String)
{
    if (String) {
        printk((char*)"[ACPI] ");
        printk((char*)String);
        printk("\n");
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

int isdigit(int c)  { return (c >= '0' && c <= '9'); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c)  { return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'); }
int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isupper(int c)  { return (c >= 'A' && c <= 'Z'); }
int islower(int c)  { return (c >= 'a' && c <= 'z'); }
int isprint(int c)  { return (c >= 0x20 && c <= 0x7E); }
int isgraph(int c)  { return (c > 0x20 && c <= 0x7E); }
int iscntrl(int c)  { return (c == 0x7F || (c >= 0 && c < 0x20)); }
int ispunct(int c)  { return isprint(c) && !isalnum(c) && !isspace(c); }
