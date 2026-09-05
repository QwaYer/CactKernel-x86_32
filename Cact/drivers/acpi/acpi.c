#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "acpi.h"
#include "cact_acpi.h"
#include "acpi_quirks.h"

static int acpi_initialized = 0;

int acpi_init(void)
{
    ACPI_STATUS status;

    /* Breadcrumbs: if the machine hard-reboots mid-init (bad SCI polarity,
     * _OSC, firmware SMI), the last printed step tells us where it died. */
    pr_info("ACPI [1/8]: initializing ACPICA subsystem");

    /*
     * Firmware AML is written and tested mostly against Windows and often
     * contains sloppy constructs on the non-Windows paths.  Pretend to be
     * Windows: enable interpreter slack (tolerate malformed AML) and report
     * a modern Windows version so firmware takes its well-tested _OSI path.
     */
    AcpiGbl_EnableInterpreterSlack = TRUE;
    AcpiGbl_OsiData = ACPI_OSI_WIN_10;
    AcpiGbl_EnableTableValidation = FALSE;

    status = AcpiInitializeSubsystem();
    if (ACPI_FAILURE(status)) {
        pr_err("  %-11s : AcpiInitializeSubsystem failed\n", "acpi");
        return -1;
    }
    pr_info("ACPI [2/8]: subsystem initialized");

    status = AcpiInitializeTables(NULL, 32, FALSE);
    if (ACPI_FAILURE(status)) {
        pr_err("  %-11s : AcpiInitializeTables failed\n", "acpi");
        return -1;
    }
    pr_info("ACPI [3/8]: tables loaded");

    status = AcpiLoadTables();
    if (ACPI_FAILURE(status)) {
        pr_warn("  %-11s : AcpiLoadTables failed (tables still available)\n", "acpi");
    } else {
        pr_info("ACPI [4/8]: namespace tables loaded");
    }

    /* SCI gets enabled here — with a wrong (edge/high) IOAPIC entry the
     * chipset can hard-reset the machine the moment events start flowing. */
    pr_info("ACPI [5/8]: enabling subsystem (SCI live)");
    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        pr_warn("  %-11s : AcpiEnableSubsystem failed (partial ACPI)\n", "acpi");
    } else {
        pr_info("ACPI [6/8]: subsystem enabled");
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        pr_warn("  %-11s : AcpiInitializeObjects failed\n", "acpi");
    } else {
        pr_info("ACPI [7/8]: namespace objects initialized");
    }

    acpi_initialized = 1;

    /*
     * _OSC on the PCIe root bridge.  Firmware on some vendors (notably HP)
     * expects the OS to evaluate _OSC with the PCIe UUID before driving the
     * PCIe hierarchy; without it the firmware keeps ACPI-level control and
     * may reboot the machine once ECAM/hotplug/AER traffic starts.
     */
    pr_info("ACPI [8/8]: evaluating _OSC");
    acpi_osc_pcie_init();

    {
        ACPI_PHYSICAL_ADDRESS rsdp_phys = AcpiOsGetRootPointer();
        if (rsdp_phys) {
            char buf[48];
            char hex[16];
            strcpy(buf, "ACPI: RSDP at 0x");
            snprintf(hex, sizeof(hex), "0x%x", (unsigned)((uint32_t)rsdp_phys));
            strcat(buf, hex);
            pr_info("%s", buf);
        } else {
            pr_warn("  %-11s : RSDP not found\n", "acpi");
        }
    }

    pr_info("ACPI: initialization complete");
    return 0;
}

int acpi_available(void)
{
    return acpi_initialized;
}

/*
 * _OSC for the PCIe root bridge.
 *
 * PCIe _OSC UUID: 33DB4D5B-1FF7-401C-9657-7441C03DD766, stored as the 16-byte
 * buffer ACPI expects (first three fields little-endian).  We evaluate _OSC on
 * the root bridge \_SB.PCI0 and request native control of the standard PCIe
 * feature set (capability structure, native hotplug, AER, PME, DPC).
 * Firmware grants only what it allows in the returned status/control buffer.
 *
 * We deliberately touch \_SB.PCI0 only: walking the whole namespace with
 * AcpiGetDevices can evaluate _OSC on child bridges/devices where firmware
 * does not expect it and crash on buggy BIOSes.
 */
#define OSC_PCIE_UUID \
    { 0x5B, 0x4D, 0xDB, 0x33, 0xF7, 0x1F, 0x1C, 0x40, \
      0x96, 0x57, 0x74, 0x41, 0xC0, 0x3D, 0xD7, 0x66 }

#define OSC_PCI_SUPPORT_MASKS            0x001Fu
#define OSC_PCI_CONTROL_MASKS            0x001Fu

static void acpi_osc_pcie_bridge(ACPI_HANDLE handle)
{
    UINT8 uuid[16] = OSC_PCIE_UUID;
    UINT8 cap[16];
    ACPI_OBJECT args[4];
    ACPI_OBJECT_LIST params;
    ACPI_BUFFER ret;
    ACPI_STATUS status;

    memset(cap, 0, sizeof(cap));
    cap[0] = (UINT8)(OSC_PCI_SUPPORT_MASKS & 0xFF);  /* dword 0: support   */
    cap[4] = (UINT8)(OSC_PCI_CONTROL_MASKS & 0xFF);  /* dword 1: control   */

    memset(args, 0, sizeof(args));
    args[0].Type = ACPI_TYPE_BUFFER;
    args[0].Buffer.Length = 16;
    args[0].Buffer.Pointer = uuid;
    args[1].Type = ACPI_TYPE_INTEGER;
    args[1].Integer.Value = 1;                       /* revision */
    args[2].Type = ACPI_TYPE_INTEGER;
    args[2].Integer.Value = 4;                       /* dword count */
    args[3].Type = ACPI_TYPE_BUFFER;
    args[3].Buffer.Length = 16;
    args[3].Buffer.Pointer = cap;

    params.Count   = 4;
    params.Pointer = args;

    ret.Length = ACPI_ALLOCATE_BUFFER;
    ret.Pointer = NULL;

    status = AcpiEvaluateObject(handle, "_OSC", &params, &ret);
    if (ACPI_FAILURE(status)) {
        pr_warn("  %-11s : _OSC failed on the PCIe root bridge\n", "acpi");
        return;
    }

    if (ret.Pointer && ((ACPI_OBJECT *)ret.Pointer)->Type == ACPI_TYPE_BUFFER) {
        UINT8 *b = ((ACPI_OBJECT *)ret.Pointer)->Buffer.Pointer;
        UINT32 cap_status = (UINT32)b[0] | ((UINT32)b[1] << 8)
                          | ((UINT32)b[2] << 16) | ((UINT32)b[3] << 24);
        UINT32 ctrl = (UINT32)b[4] | ((UINT32)b[5] << 8)
                    | ((UINT32)b[6] << 16) | ((UINT32)b[7] << 24);
        pr_info("ACPI: _OSC granted PCIe control 0x%x (status 0x%x)",
                (unsigned)ctrl, (unsigned)cap_status);
    }

    if (ret.Pointer)
        AcpiOsFree(ret.Pointer);
}

void acpi_osc_pcie_init(void)
{
    ACPI_HANDLE handle = NULL;
    ACPI_STATUS status;

    if (!acpi_initialized)
        return;

    /* Manufacturer quirk: some boards ship a broken _OSC method. */
    if (acpi_board_quirks() & ACPI_QUIRK_DISABLE_OSC) {
        pr_info("ACPI: _OSC skipped (board quirk: broken _OSC AML)");
        return;
    }

    status = AcpiGetHandle(ACPI_ROOT_OBJECT, "\\_SB.PCI0", &handle);
    if (ACPI_FAILURE(status) || !handle) {
        pr_warn("  %-11s : \\_SB.PCI0 not found — _OSC skipped\n", "acpi");
        return;
    }

    pr_info("ACPI: evaluating _OSC on \\_SB.PCI0");
    acpi_osc_pcie_bridge(handle);
}
