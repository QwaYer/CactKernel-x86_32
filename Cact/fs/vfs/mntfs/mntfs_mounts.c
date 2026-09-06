#include "mntfs.h"
#include "mntfs_internal.h"
#include "etcfs.h"
#include "devfs.h"
#include "procfs.h"
#include "tmpfs.h"
#include "binfs.h"
#include "sbinfs.h"
#include "libfs.h"
#include "varfs.h"
#include "usrfs.h"
#include "vfs.h"
#include "fs_mod.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "blkdev.h"

// Append device name to /etc/mounts.
void _mounts_add(const char *devname) {
    char cur[1024]; memset(cur, 0, sizeof(cur));
    int len = etcfs_read("mounts", cur, sizeof(cur) - 64);
    if (len < 0) len = 0;
    int dlen = 0; while (devname[dlen]) dlen++;
    for (int i = 0; i <= len - dlen; i++) {
        int m = 1;
        for (int j = 0; j < dlen; j++) if (cur[i+j] != devname[j]) { m=0; break; }
        if (m && (cur[i+dlen]=='\n'||cur[i+dlen]=='\0')) return; // already there
    }
    int p = len;
    for (int i = 0; devname[i] && p < (int)sizeof(cur)-2; i++) cur[p++] = devname[i];
    cur[p++]='\n'; cur[p]='\0';
    etcfs_write("mounts", cur, (uint32_t)p);
}

// Remove device name from /etc/mounts.
void _mounts_remove(const char *devname) {
    char cur[1024]; memset(cur, 0, sizeof(cur));
    int len = etcfs_read("mounts", cur, sizeof(cur) - 1);
    if (len <= 0) return;
    cur[len] = '\0';
    char out[1024]; int oi=0, i=0;
    int dlen=0; while (devname[dlen]) dlen++;
    while (i < len) {
        int start=i;
        while (i<len && cur[i]!='\n') i++;
        int end=i; if (i<len) i++;
        int llen=end-start;
        int match=(llen==dlen);
        if (match) for (int k=0;k<llen;k++) if (cur[start+k]!=devname[k]) { match=0; break; }
        if (!match) {
            for (int k=start; k<end && oi<(int)sizeof(out)-2; k++) out[oi++]=cur[k];
            if (oi<(int)sizeof(out)-1) out[oi++]='\n';
        }
    }
    out[oi]='\0';
    etcfs_write("mounts", out, (uint32_t)oi);
}

// Mount devices listed in /etc/mounts.
void _mounts_mount_all(void) {
    char buf[1024]; memset(buf, 0, sizeof(buf));
    int len = etcfs_read("mounts", buf, sizeof(buf) - 1);
    if (len <= 0) { printk("[mounts] empty\n"); return; }
    buf[len] = '\0';
    int i = 0;
    while (i < len) {
        char line[32]; int li=0;
        while (i<len && buf[i]!='\n' && li<31) line[li++]=buf[i++];
        line[li]='\0'; if (i<len) i++;
        while (li>0 && (line[li-1]==' '||line[li-1]=='\r')) line[--li]='\0';
        if (li==0 || line[0]=='#') continue;
        if (_find_disk(line)) continue; // already mounted
        blkdev_t *bd = blkdev_find(line);
        if (!bd) {
            printk("[mounts] unknown: "); printk(line); printk("\n"); continue;
        }
        vfs_node_t *node = fs_mod_mount(bd);
        if (!node) { printk("[mounts] failed: "); printk(line); printk("\n"); continue; }
        mntfs_mount_disk(line, node, 0);
        printk("[mounts] "); printk(line); printk(" mounted\n");
    }
}
