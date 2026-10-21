#include "devfs.h"
#include "devfs_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "pipe.h"
#include "pci_driver.h"
#include "mouse.h"
#include "fb.h"
#include "validate.h"
#include "multiboot2.h"

// ── Built-in driver tables ────────────────────────────────────────────────

static int _null_read (void *p, uint32_t o, uint32_t s, char *b)
    { (void)p;(void)o;(void)s;(void)b; return 0; }
static int _null_write(void *p, uint32_t o, uint32_t s, char *b)
    { (void)p;(void)o;(void)b; return (int)s; }
devfs_driver_t drv_null = { .read=_null_read, .write=_null_write };

static int _zero_read(void *p, uint32_t o, uint32_t s, char *b)
    { (void)p;(void)o; memset(b,0,s); return (int)s; }
devfs_driver_t drv_zero = { .read=_zero_read, .write=_null_write };

// /dev/random + /dev/urandom — the kernel CSPRNG (ChaCha20 DRBG, cact_crypto).
// This is where getrandom(2), the crypto service and TLS key generation all get
// their bytes, so there is exactly one generator to trust.
extern int cact_csprng_fill(uint8_t *buf, uint32_t len);

static int _rand_read(void *p, uint32_t o, uint32_t s, char *b) {
    (void)p;(void)o;
    if (cact_csprng_fill((uint8_t *)b, s) != 0) return -1;
    return (int)s;
}
devfs_driver_t drv_random = { .read=_rand_read, .write=_null_write };

extern int keyboard_read_char(void);

// /dev/tty0 + /dev/ttyN are not devfs's business: the terminal driver owns its
// devfs_driver_t and registers every VT node itself (Cact/drivers/tty/tty.c).

// /dev/keyboard — raw character stream from keyboard circular buffer
static int _kbd_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;(void)off;
    uint32_t i=0;
    while(i<size){
        int c;
        while((c=keyboard_read_char())<0) schedule();
        buf[i++]=(char)c;
        if(size<=1) break;
    }
    return (int)i;
}
devfs_driver_t drv_keyboard = { .read=_kbd_read };

// /dev/mouse — raw mouse event packets (mouse_packet_t structs)
static int _mouse_read_dev(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;(void)off;
    if(size<sizeof(mouse_packet_t)) return -1;
    mouse_packet_t pkt;
    while(mouse_read_event(&pkt)<0) schedule();
    memcpy(buf,&pkt,sizeof(mouse_packet_t));
    return (int)sizeof(mouse_packet_t);
}
devfs_driver_t drv_mouse = { .read=_mouse_read_dev };

// /dev/fb0 — framebuffer device (read/write at byte offset, ioctl for screen info)
#define FBIOGET_VSCREENINFO 0x4600
struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
};

static int _fb_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    uint32_t fb_sz = fb_get_pitch() * fb_get_height();
    if(off>=fb_sz) return 0;
    if(off+size>fb_sz) size=fb_sz-off;
    memcpy(buf, (char*)fb_get_buffer()+off, size);
    return (int)size;
}

static int _fb_write(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    uint32_t fb_sz = fb_get_pitch() * fb_get_height();
    if(off>=fb_sz) return 0;
    if(off+size>fb_sz) size=fb_sz-off;
    memcpy((char*)fb_get_buffer()+off, buf, size);
    fb_flush();
    return (int)size;
}

static int _fb_ioctl(void *p, uint32_t cmd, void *arg) {
    (void)p;
    if(cmd==FBIOGET_VSCREENINFO) {
        if(!validate_user_ptr(arg, sizeof(struct fb_var_screeninfo))) return -1;
        struct fb_var_screeninfo *info = (struct fb_var_screeninfo*)arg;
        info->xres          = fb_get_width();
        info->yres          = fb_get_height();
        info->xres_virtual  = fb_get_width();
        info->yres_virtual  = fb_get_height();
        info->xoffset       = 0;
        info->yoffset       = 0;
        info->bits_per_pixel = fb_get_bpp();
        info->grayscale     = 0;
        return 0;
    }
    return -1;
}
devfs_driver_t drv_fb = {
    .read=_fb_read, .write=_fb_write, .ioctl=_fb_ioctl
};

// /dev/localrepo, /dev/cctkfs — the two Multiboot2 modules GRUB hands over:
// the compressed localrepo archive and the cctkfs image.  Both are far too big
// to stage a second time, so they stay in place in physical memory (RAM is
// identity-mapped) and are served read-only.  cgoct copies the archive into
// tmpfs at boot and firmextr/cact-install stream firmware out of it; the
// installer lays the cctkfs image onto the target straight from /dev/cctkfs,
// which is why the image must not be bundled inside itself.
static int _module_read(const mb2_module_info_t *mod, uint32_t off,
                        uint32_t size, char *buf) {
    uint32_t total = mod->mod_size;
    if (!total || off >= total) return 0;
    if (size > total - off) size = total - off;
    memcpy(buf, (const void *)(uintptr_t)(mod->mod_start + off), size);
    return (int)size;
}

static int _localrepo_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    return _module_read(&mb2_localrepo_module, off, size, buf);
}

static int _cctkfs_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    return _module_read(&mb2_cctkfs_module, off, size, buf);
}

devfs_driver_t drv_localrepo = { .read=_localrepo_read };
devfs_driver_t drv_cctkfs    = { .read=_cctkfs_read };

// Register this file's devices.  Each driver group registers the nodes it
// owns, so devfs_init() stays a generic bootstrap and carries no device list.
void devfs_devices_init(void) {
    register_chrdev("null",    DEVFS_F_CHAR, &drv_null,   0);
    register_chrdev("zero",    DEVFS_F_CHAR, &drv_zero,   0);
    register_chrdev("random",  DEVFS_F_CHAR, &drv_random, 0);
    register_chrdev("urandom", DEVFS_F_CHAR, &drv_random, 0);

    // Input devices live under /dev/input (Linux convention); register_chrdev()
    // also publishes /dev/keyboard and /dev/mouse as compat symlinks.
    register_chrdev("input/keyboard", DEVFS_F_CHAR, &drv_keyboard, 0);
    register_chrdev("input/mouse",    DEVFS_F_CHAR, &drv_mouse,    0);
    register_chrdev("fb0",      DEVFS_F_CHAR, &drv_fb,       0);

    // /dev/localrepo serves whatever GRUB handed over; report its size so a
    // boot medium without the `module2 … localrepo` line — or with a module
    // that turned out to be empty — is visible in the log instead of surfacing
    // as an empty archive at firmextr's first read.
    if (mb2_localrepo_module.mod_start)
        pr_info("%s: localrepo module %s, %u KiB (served as /dev/localrepo)\n",
                "devfs", mb2_localrepo_module.name,
                (unsigned)(mb2_localrepo_module.mod_size / 1024));
    else
        pr_warn("%s: no localrepo module — /dev/localrepo reads empty\n", "devfs");

    if (mb2_cctkfs_module.mod_start)
        pr_info("%s: cctkfs module %s, %u KiB (served as /dev/cctkfs)\n",
                "devfs", mb2_cctkfs_module.name,
                (unsigned)(mb2_cctkfs_module.mod_size / 1024));

    devfs_entry_t *lr =
        register_chrdev("localrepo", DEVFS_F_CHAR, &drv_localrepo, 0);
    // Publish the module size on the node: with no module it stays 0, which is
    // how cgoct/cact-install tell "this device carries no archive" (they skip
    // zero-size sources) instead of copying an empty file out of it.
    if (lr)
        lr->node.size = mb2_localrepo_module.mod_size;

    // /dev/cctkfs serves the boot cctkfs image in place, so the installer can
    // put the image being booted — not a stale copy bundled inside it — onto a
    // target disk.  Same "size 0 means no module" contract as /dev/localrepo.
    devfs_entry_t *cf =
        register_chrdev("cctkfs", DEVFS_F_CHAR, &drv_cctkfs, 0);
    if (cf)
        cf->node.size = mb2_cctkfs_module.mod_size;
}
