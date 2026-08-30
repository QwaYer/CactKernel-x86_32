#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "acpi.h"
#include "cact_acpi.h"

static int acpi_initialized = 0;

int acpi_init(void)
{
    ACPI_STATUS status;

    pr_info("ACPI: initializing ACPICA subsystem");

    /*
     * Firmware AML is written and tested mostly against Windows and often
     * contains sloppy constructs on the non-Windows paths.  Pretend to be
     * Windows: enable interpreter slack (tolerate malformed AML) and report
     * a modern Windows version so firmware takes its well-tested _OSI path.
     */
    AcpiGbl_EnableInterpreterSlack = TRUE;
    AcpiGbl_OsiData = ACPI_OSI_WIN_10;

    status = AcpiInitializeSubsystem();
    if (ACPI_FAILURE(status)) {
        pr_err("ACPI: AcpiInitializeSubsystem failed");
        return -1;
    }
    pr_info("ACPI: subsystem initialized");

    status = AcpiInitializeTables(NULL, 32, FALSE);
    if (ACPI_FAILURE(status)) {
        pr_err("ACPI: AcpiInitializeTables failed");
        return -1;
    }
    pr_info("ACPI: tables loaded");

    status = AcpiLoadTables();
    if (ACPI_FAILURE(status)) {
        pr_warn("ACPI: AcpiLoadTables failed (tables still available)");
    } else {
        pr_info("ACPI: namespace tables loaded");
    }

    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        pr_warn("ACPI: AcpiEnableSubsystem failed (partial ACPI)");
    } else {
        pr_info("ACPI: subsystem enabled");
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        pr_warn("ACPI: AcpiInitializeObjects failed");
    } else {
        pr_info("ACPI: namespace objects initialized");
    }

    acpi_initialized = 1;

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
            pr_warn("ACPI: RSDP not found");
        }
    }

    pr_info("ACPI: initialization complete");
    return 0;
}

int acpi_available(void)
{
    return acpi_initialized;
}
