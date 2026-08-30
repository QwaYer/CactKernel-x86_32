#ifndef DEVFS_INTERNAL_H
#define DEVFS_INTERNAL_H

#include "devfs.h"

/* devfs_devices.c — built-in driver tables. */
extern devfs_driver_t drv_null;
extern devfs_driver_t drv_zero;
extern devfs_driver_t drv_random;
extern devfs_driver_t drv_disk;
extern devfs_driver_t drv_tty;
extern devfs_driver_t drv_keyboard;
extern devfs_driver_t drv_mouse;
extern devfs_driver_t drv_fb;

#endif
