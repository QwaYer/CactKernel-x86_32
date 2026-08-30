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

ACPI_STATUS AcpiOsReadPort(
    ACPI_IO_ADDRESS         Address,
    UINT32                  *Value,
    UINT32                  Width)
{
    switch (Width) {
    case 8:  *Value = inb((UINT16)Address); break;
    case 16: *Value = inw((UINT16)Address); break;
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
    case 8:  outb((UINT16)Address, (UINT8)Value); break;
    case 16: outw((UINT16)Address, (UINT16)Value); break;
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
    UINT32 val = pci_read_config_dword(PciId->Bus, PciId->Device, PciId->Function,
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
    UINT32 val = pci_read_config_dword(PciId->Bus, PciId->Device, PciId->Function,
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
    pci_write_config_dword(PciId->Bus, PciId->Device, PciId->Function, (UINT8)(Reg & 0xFC), val);
    return AE_OK;
}
