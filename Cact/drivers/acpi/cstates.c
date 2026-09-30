/*
 * ACPI _CST probe: read the processor C-state descriptors and publish them to
 * the scheduler's energy model (sched/src/cstate.rs).
 *
 * The scheduler enters a state with MONITOR/MWAIT, so only entries whose
 * Register is in the "fixed hardware" address space (0x7F) are usable — there
 * the Address field *is* the MWAIT hint (EAX).  Entries with a system-I/O or
 * FFH register would need the ACPI C-state write path, which this kernel does
 * not implement, so they are ignored.
 *
 * C-state ids published here must match sched/src/cstate.rs: 1 = C1, 2 = C3,
 * 3 = C6.  ACPI C-state types are 1 = C1, 2 = C2, 3 = C3, >= 4 = the deepest
 * states (C4/C6/...); we map C2 away (no slot) and >= 4 onto our C6.
 */
#include "acpi.h"
#include "cact_acpi.h"
#include "kernel.h"   /* pr_info / KERN_INFO */
#include "klib.h"

/* Filled in by the scheduler (Rust).  Declares the sink for the parsed data. */
extern int energy_cstate_configure(uint32_t state, int available,
                                   uint32_t latency_us, int mwait_hint);

#define ACPI_ADR_SPACE_FIXED_HW 0x7F

static int cst_state_for_type(uint32_t type)
{
    if (type == 1) return 1;        /* C1                                     */
    if (type == 3) return 2;        /* C3                                     */
    if (type >= 4) return 3;        /* C4/C6/... -> our deepest modelled (C6) */
    return -1;                      /* C2 and anything else: no slot          */
}

/* Evaluate _CST on one processor object.  Returns the number of C-states it
 * published, or -1 when the object has no usable _CST. */
static int cst_configure_handle(ACPI_HANDLE handle)
{
    ACPI_BUFFER ret;
    ACPI_STATUS status;

    ret.Length  = ACPI_ALLOCATE_BUFFER;
    ret.Pointer = NULL;

    status = AcpiEvaluateObject(handle, "_CST", NULL, &ret);
    if (ACPI_FAILURE(status) || !ret.Pointer)
        return -1;

    ACPI_OBJECT *pkg = (ACPI_OBJECT *)ret.Pointer;
    if (pkg->Type != ACPI_TYPE_PACKAGE) {
        AcpiOsFree(ret.Pointer);
        return -1;
    }

    int configured = 0;
    for (UINT32 i = 0; i < pkg->Package.Count; i++) {
        ACPI_OBJECT *e = &pkg->Package.Elements[i];
        if (e->Type != ACPI_TYPE_PACKAGE || e->Package.Count < 4)
            continue;

        ACPI_OBJECT *te = &e->Package.Elements[0];   /* C-state type   */
        ACPI_OBJECT *le = &e->Package.Elements[1];   /* latency (usec) */
        ACPI_OBJECT *re = &e->Package.Elements[3];   /* register       */

        if (te->Type != ACPI_TYPE_INTEGER || le->Type != ACPI_TYPE_INTEGER)
            continue;

        uint32_t type    = (uint32_t)te->Integer.Value;
        uint32_t latency = (uint32_t)le->Integer.Value;
        int      hint    = -1;

        if (re->Type == ACPI_TYPE_BUFFER && re->Buffer.Length >= 12) {
            uint8_t *b = re->Buffer.Pointer;
            uint64_t addr = (uint64_t)b[4] | ((uint64_t)b[5] << 8) |
                            ((uint64_t)b[6] << 16) | ((uint64_t)b[7] << 24);
            if (b[0] == ACPI_ADR_SPACE_FIXED_HW && addr != 0)
                hint = (int)(uint32_t)addr;
        } else if (re->Type == ACPI_TYPE_INTEGER && re->Integer.Value != 0) {
            /* A few firmwares encode the hint directly as an integer. */
            hint = (int)(uint32_t)re->Integer.Value;
        }

        int state = cst_state_for_type(type);
        if (state < 0)
            continue;

        /* Deep states are only usable with an MWAIT hint; C1 always is (hint 0
         * is HLT).  Skip a deep entry the firmware did not give a hint for. */
        if (state != 1 && hint < 0)
            continue;

        pr_info("ACPI: _CST type %u -> our C%s, MWAIT hint 0x%x, latency %u us",
                (unsigned)type,
                state == 2 ? "3" : (state == 3 ? "6" : "1"),
                (unsigned)(hint < 0 ? 0 : hint), (unsigned)latency);

        energy_cstate_configure((uint32_t)state, 1, latency, hint);
        configured++;
    }

    AcpiOsFree(ret.Pointer);
    return configured;
}

/*
 * Namespace walk.  We deliberately do NOT evaluate _HID: AcpiGetDevices does,
 * and its _HID repair path (`AcpiNsRepair_HID`) dereferences the firmware's
 * _HID string — which faults on some boards (observed: a kernel page fault at
 * boot on a machine whose _OSC is already quirks-listed).  Instead we walk the
 * namespace and only look the child `_CST` up by name (`AcpiGetHandle`, a pure
 * namespace lookup, no AML execution), evaluating it only when it exists.
 */
static ACPI_STATUS cst_walk_cb(ACPI_HANDLE object, UINT32 level,
                               void *context, void **return_value)
{
    (void)level;
    (void)return_value;

    int *done = (int *)context;
    if (*done)
        return AE_CTRL_TERMINATE;   /* C-states are uniform across cores */

    ACPI_HANDLE cst = NULL;
    if (ACPI_SUCCESS(AcpiGetHandle(object, "_CST", &cst))) {
        if (cst_configure_handle(object) > 0)
            *done = 1;
    }
    return AE_OK;
}

/* Walk the namespace and read _CST from the first object that has it.
 * Returns 0 when a processor was configured, -1 otherwise. */
int acpi_cstates_probe(void)
{
    int         done = 0;
    ACPI_STATUS status;

    if (!acpi_available())
        return -1;

    status = AcpiWalkNamespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT,
                               ACPI_UINT32_MAX, cst_walk_cb, NULL, &done, NULL);
    if (ACPI_FAILURE(status) && status != AE_CTRL_TERMINATE)
        return -1;

    return done ? 0 : -1;
}
