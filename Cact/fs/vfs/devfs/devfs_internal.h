#ifndef DEVFS_INTERNAL_H
#define DEVFS_INTERNAL_H

#include "devfs.h"

/* devfs_devices.c — built-in driver tables. */
extern devfs_driver_t drv_null;
extern devfs_driver_t drv_zero;
extern devfs_driver_t drv_random;
extern devfs_driver_t drv_tty;
extern devfs_driver_t drv_keyboard;
extern devfs_driver_t drv_mouse;
extern devfs_driver_t drv_fb;

/* devfs_services.c — kernel-service devices (/dev/console, /dev/sys, ...). */
extern devfs_driver_t drv_console;
extern devfs_driver_t drv_sys;
extern devfs_driver_t drv_net;
extern devfs_driver_t drv_pipe;
extern devfs_driver_t drv_kmsg;
extern devfs_driver_t drv_memfd;
extern devfs_driver_t drv_eventfd;
extern devfs_driver_t drv_timerfd;
extern devfs_driver_t drv_signalfd;
extern devfs_driver_t drv_epoll;

/* devfs_crypto.c — /dev/crypto kernel crypto service. */
extern devfs_driver_t drv_crypto;

/* devfs_proc.c — per-process nodes (/dev/tty, /dev/stdin, /dev/fd, /dev/core)
 * and the pty namespace (/dev/ptmx, /dev/pts).  Called once from devfs_init();
 * the nodes it publishes must outlive the call. */
void devfs_proc_init(void);

#endif
