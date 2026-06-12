#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "acpi.h"
#include "cact_acpi.h"

static int acpi_initialized = 0;

int acpi_init(void)
{
    ACPI_STATUS status;

    klog(LOG_OK, "ACPI: initializing ACPICA subsystem");

    status = AcpiInitializeSubsystem();
    if (ACPI_FAILURE(status)) {
        klog(LOG_ERROR, "ACPI: AcpiInitializeSubsystem failed");
        return -1;
    }
    klog(LOG_OK, "ACPI: subsystem initialized");

    status = AcpiInitializeTables(NULL, 32, FALSE);
    if (ACPI_FAILURE(status)) {
        klog(LOG_ERROR, "ACPI: AcpiInitializeTables failed");
        return -1;
    }
    klog(LOG_OK, "ACPI: tables loaded");

    status = AcpiLoadTables();
    if (ACPI_FAILURE(status)) {
        klog(LOG_WARN, "ACPI: AcpiLoadTables failed (tables still available)");
    } else {
        klog(LOG_OK, "ACPI: namespace tables loaded");
    }

    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        klog(LOG_WARN, "ACPI: AcpiEnableSubsystem failed (partial ACPI)");
    } else {
        klog(LOG_OK, "ACPI: subsystem enabled");
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        klog(LOG_WARN, "ACPI: AcpiInitializeObjects failed");
    } else {
        klog(LOG_OK, "ACPI: namespace objects initialized");
    }

    acpi_initialized = 1;

    {
        ACPI_PHYSICAL_ADDRESS rsdp_phys = AcpiOsGetRootPointer();
        if (rsdp_phys) {
            char buf[48];
            char hex[16];
            strcpy(buf, "ACPI: RSDP at 0x");
            hex_to_ascii((uint32_t)rsdp_phys, hex);
            strcat(buf, hex);
            klog(LOG_OK, buf);
        } else {
            klog(LOG_WARN, "ACPI: RSDP not found");
        }
    }

    klog(LOG_OK, "ACPI: initialization complete");
    return 0;
}

int acpi_available(void)
{
    return acpi_initialized;
}
