/*
 * SMBIOS/DMI probing (board identification).
 *
 * The SMBIOS entry point lives in the firmware BIOS area between 0xF0000 and
 * 0xFFFFF, exactly like the ACPI RSDP, and the same temporary physical map
 * helpers are used to reach it.  Two entry point formats are accepted:
 *
 *   2.1+  anchor "_SM_"  (contains the 16-bit structure table length)
 *   3.x   anchor "_SM3_" (64-bit table address; length from the maximum
 *         structure-table size field)
 *
 * Structure types 0 (BIOS information) and 1 (system information) are walked
 * out of the table so the vendor, product name and BIOS version can be matched
 * against the firmware a board quirk was written for.
 */

#include "kernel.h"
#include "klib.h"
#include "cact_acpi.h"
#include "dmi.h"

#define DMI_STR_MAX         64u

#define SMBIOS_SCAN_START   0x000F0000u
#define SMBIOS_SCAN_LEN     0x00010000u   /* 0xF0000..0xFFFFF */
#define SMBIOS_SCAN_STEP    16u
#define SMBIOS_TABLE_MAX    0x00010000u   /* 64 KB safety cap */

#define SMBIOS_END_TYPE     127u

static char dmi_vendor[DMI_STR_MAX];
static char dmi_product[DMI_STR_MAX];
static char dmi_bios_version[DMI_STR_MAX];
static int  dmi_state;                    /* 0 = unprobed, 1 = ok, -1 = absent */

static int dmi_checksum(const uint8_t *p, unsigned int n)
{
    unsigned int sum = 0;
    for (unsigned int i = 0; i < n; i++)
        sum += p[i];
    return (sum & 0xFF) == 0;
}

static uint16_t dmi_get16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t dmi_get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int dmi_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static int dmi_contains(const char *haystack, const char *needle)
{
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*n && *h && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return 1;
    }
    return 0;
}

/* Copy the idx-th string (1-based) of a structure into @out.  Index 0 or a
 * missing string yields an empty string.  Reads never leave the table. */
static void dmi_struct_string(const uint8_t *st, const uint8_t *end,
                              uint8_t idx, char *out, uint32_t cap)
{
    const uint8_t *s = st + st[1];
    uint32_t n = 0;

    out[0] = '\0';
    if (idx == 0 || s >= end || cap == 0)
        return;

    for (uint8_t i = 1; i < idx; i++) {
        while (s < end && *s)
            s++;
        if (s >= end)
            return;
        s++;                                /* skip the string's NUL */
    }

    while (s < end && *s && n + 1 < cap) {
        out[n++] = (char)*s;
        s++;
    }
    out[n] = '\0';
}

/* Advance past the formatted area and the NUL-terminated string set (ending
 * in a double NUL), rounded up to the next word boundary. */
static const uint8_t *dmi_next_structure(const uint8_t *st, const uint8_t *end)
{
    const uint8_t *q = st + st[1];
    if (q >= end)
        return end;

    while (q < end) {
        if (*q == 0) {
            q++;
            break;
        }
        while (q < end && *q != 0)
            q++;
        if (q >= end)
            break;
        q++;                                /* skip the string's NUL */
    }
    if (q >= end)
        return end;

    q = (const uint8_t *)(((uintptr_t)q + 1u) & ~(uintptr_t)1u);
    return (q > end) ? end : q;
}

static int dmi_locate_eps(uint32_t *addr_out, uint32_t *len_out)
{
    void *m = acpi_temp_map(SMBIOS_SCAN_START, SMBIOS_SCAN_LEN);
    if (!m)
        return 0;

    const uint8_t *p = m;
    int found = 0;

    for (uint32_t off = 0; off + 0x20 <= SMBIOS_SCAN_LEN; off += SMBIOS_SCAN_STEP) {
        if (p[off] != '_' || p[off + 1] != 'S' || p[off + 2] != 'M')
            continue;

        if (p[off + 3] == '_') {
            /* SMBIOS 2.1+ entry point ("_SM_" .. "_DMI_"). */
            uint8_t len = p[off + 5];
            uint32_t addr, table_len;

            if (len < 0x1E || len > 0x20 || !dmi_checksum(p + off, len))
                continue;
            if (p[off + 0x10] != '_' || p[off + 0x11] != 'D' ||
                p[off + 0x12] != 'M' || p[off + 0x13] != 'I' ||
                p[off + 0x14] != '_')
                continue;
            if (!dmi_checksum(p + off + 0x10, 15))
                continue;

            table_len = dmi_get16(p + off + 0x16);
            addr      = dmi_get32(p + off + 0x18);
            if (addr == 0 || table_len == 0 || table_len > SMBIOS_TABLE_MAX)
                continue;

            *addr_out = addr;
            *len_out  = table_len;
            found = 1;
            break;
        }

        if (p[off + 3] == '3' && p[off + 4] == '_') {
            /* SMBIOS 3.x entry point ("_SM3_"): 5-byte anchor, checksum at
             * +5, length at +6, max structure-table size at +0x0C and the
             * 64-bit structure table address at +0x10. */
            uint8_t len = p[off + 6];
            uint32_t addr, addr_hi, max_size;

            if (len < 0x18 || !dmi_checksum(p + off, len))
                continue;

            max_size = dmi_get32(p + off + 0x0C);
            addr     = dmi_get32(p + off + 0x10);
            addr_hi  = dmi_get32(p + off + 0x14);
            if (addr_hi != 0 || addr == 0 ||
                max_size == 0 || max_size > SMBIOS_TABLE_MAX)
                continue;

            *addr_out = addr;
            *len_out  = max_size;
            found = 1;
            break;
        }
    }

    acpi_temp_unmap(m, SMBIOS_SCAN_LEN);
    return found;
}

static int dmi_parse_table(const uint8_t *table, uint32_t len)
{
    const uint8_t *p = table;
    const uint8_t *end = table + len;
    int got_type0 = 0, got_type1 = 0;

    while ((uint32_t)(end - p) >= 4u) {
        uint8_t type = p[0];
        uint8_t fmt  = p[1];

        if (type == SMBIOS_END_TYPE || fmt < 4u || (uint32_t)(end - p) < fmt)
            break;

        if (type == 1 && fmt >= 6u && !got_type1) {
            /* System information: 0x04 manufacturer, 0x05 product name. */
            dmi_struct_string(p, end, p[4], dmi_vendor, DMI_STR_MAX);
            dmi_struct_string(p, end, p[5], dmi_product, DMI_STR_MAX);
            got_type1 = 1;
        }
        if (type == 0 && fmt >= 6u && !got_type0) {
            /* BIOS information: 0x05 BIOS version. */
            dmi_struct_string(p, end, p[5], dmi_bios_version, DMI_STR_MAX);
            got_type0 = 1;
        }
        if (got_type1 && got_type0)
            break;

        p = dmi_next_structure(p, end);
        if (p >= end)
            break;
    }

    return got_type1 && dmi_product[0] != '\0' && dmi_vendor[0] != '\0';
}

static int dmi_collect_once(void)
{
    if (dmi_state != 0)
        return dmi_state;

    dmi_state = -1;

    uint32_t addr = 0, len = 0;
    if (dmi_locate_eps(&addr, &len)) {
        void *m = acpi_temp_map(addr, len);
        if (m) {
            if (dmi_parse_table((const uint8_t *)m, len))
                dmi_state = 1;
            acpi_temp_unmap(m, len);
        }
    }

    if (dmi_state == 1)
        pr_info("DMI: %s / %s (BIOS %s)",
                dmi_vendor, dmi_product, dmi_bios_version);

    return dmi_state;
}

int dmi_is_hp_290g1(void)
{
    if (dmi_collect_once() <= 0)
        return 0;

    /* Manufacturer is "HP" / "HP Inc." on modern boards, "Hewlett-Packard"
     * on older ones. */
    if (strcmp(dmi_vendor, "Hewlett-Packard") != 0 &&
        !dmi_prefix(dmi_vendor, "HP"))
        return 0;

    /* HP 290 G1 SFF / Microtower (Intel B360). */
    if (!dmi_contains(dmi_product, "290 G1"))
        return 0;

    return 1;
}
