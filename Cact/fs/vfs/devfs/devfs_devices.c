#include "devfs.h"
#include "devfs_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "pipe.h"
#include "blkdev.h"
#include "pci_driver.h"
#include "mouse.h"
#include "fb.h"
#include "validate.h"

// ── Built-in driver tables ────────────────────────────────────────────────

static int _null_read (void *p, uint32_t o, uint32_t s, char *b)
    { (void)p;(void)o;(void)s;(void)b; return 0; }
static int _null_write(void *p, uint32_t o, uint32_t s, char *b)
    { (void)p;(void)o;(void)b; return (int)s; }
devfs_driver_t drv_null = { .read=_null_read, .write=_null_write };

static int _zero_read(void *p, uint32_t o, uint32_t s, char *b)
    { (void)p;(void)o; memset(b,0,s); return (int)s; }
devfs_driver_t drv_zero = { .read=_zero_read, .write=_null_write };

static uint32_t _rng = 0xDEADC0DE;
static uint32_t _lcg(void) { _rng=_rng*1664525u+1013904223u; return _rng; }
static int _rand_read(void *p, uint32_t o, uint32_t s, char *b) {
    (void)p;(void)o;
    uint32_t i=0;
    while(i<s){ uint32_t r=_lcg(); uint32_t c=s-i; if(c>4)c=4;
                memcpy(b+i,&r,c); i+=c; }
    return (int)s;
}
devfs_driver_t drv_random = { .read=_rand_read, .write=_null_write };

static int _disk_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    uint8_t sector_buf[512];
    uint32_t lba=off/512, written=0;
    while(written<size){
        blkdev_read_sector(lba,(uint8_t*)sector_buf);
        uint32_t c=512; if(c>size-written)c=size-written;
        memcpy(buf+written,sector_buf,c);
        written+=c; lba++;
    }
    return (int)written;
}
static int _disk_write(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;
    uint8_t sector_buf[512];
    uint32_t lba=off/512, written=0;
    while(written<size){
        blkdev_read_sector(lba,(uint8_t*)sector_buf);
        uint32_t c=512; if(c>size-written)c=size-written;
        memcpy(sector_buf,buf+written,c);
        blkdev_write_sector(lba,(uint8_t*)sector_buf);
        written+=c; lba++;
    }
    return (int)written;
}
static int _disk_ctl(void *p, const char *cmd, uint32_t len) {
    (void)p;
    if (len < 2) return -1;
    if(cmd[0]=='f'&&cmd[1]=='l') { printk("[disk] flush (noop)\n"); return 0; }
    printk("[disk] unknown ctl: "); printk((char*)cmd); printk("\n");
    return -1;
}
static int _disk_status(void *p, char *buf, uint32_t size) {
    (void)p;
    blkdev_t *boot = blkdev_get_boot();
    const char *h = "device: ";
    uint32_t n=0;
    while(h[n]&&n<size-1){buf[n]=h[n];n++;}
    if (boot) { for(int i=0;boot->name[i]&&n<size-1;i++) buf[n++]=boot->name[i]; }
    else { const char *u="none"; for(int i=0;u[i]&&n<size-1;i++) buf[n++]=u[i]; }
    if(n<size-1) buf[n++]='\n';
    const char *t = "type: block (via blkdev)\n";
    for(int i=0;t[i]&&n<size-1;i++) buf[n++]=t[i];
    buf[n]='\0';
    return (int)n;
}
devfs_driver_t drv_disk = {
    .read=_disk_read, .write=_disk_write,
    .ctl=_disk_ctl,   .status=_disk_status
};

extern int keyboard_read_char(void);

static int _tty_read(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;(void)off;
    uint32_t i=0;
    while(i<size){
        int c;
        while((c=keyboard_read_char())<0) schedule();
        buf[i++]=(char)c;
        /* One syscall = one key for size==1 (readline); larger reads stay line-oriented. */
        if (size <= 1) break;
        if((char)c=='\n') break;
    }
    return (int)i;
}
static int _tty_write(void *p, uint32_t off, uint32_t size, char *buf) {
    (void)p;(void)off;
    if (!buf || !validate_user_ptr(buf, size)) return -1;
    char tmp[256]; uint32_t i=0;
    while(i<size){
        uint32_t c=size-i; if(c>=sizeof(tmp))c=sizeof(tmp)-1;
        memcpy(tmp,buf+i,c); tmp[c]='\0';
        printk(tmp); i+=c;
    }
    return (int)size;
}
static int _tty_status(void *p, char *buf, uint32_t size) {
    (void)p;
    const char *s = "device: tty\ntype: char\n";
    uint32_t n=0; while(s[n]&&n<size-1){buf[n]=s[n];n++;} buf[n]='\0';
    return (int)n;
}
devfs_driver_t drv_tty = {
    .read=_tty_read, .write=_tty_write, .status=_tty_status
};

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
        info->bits_per_pixel = 32;
        info->grayscale     = 0;
        return 0;
    }
    return -1;
}
devfs_driver_t drv_fb = {
    .read=_fb_read, .write=_fb_write, .ioctl=_fb_ioctl
};
