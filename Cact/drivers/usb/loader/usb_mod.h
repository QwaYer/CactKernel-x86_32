#ifndef USB_MOD_H
#define USB_MOD_H

#include <stdint.h>

/* Loadable USB driver modules (.cctk).
 *
 * A USB driver module is a relocatable ET_REL image that exports:
 *     int  usb_driver_init(void);   // required: register the driver(s)
 *     void usb_driver_exit(void);   // optional teardown
 * usb_driver_init() calls the exported usb_driver_register() with a static
 * usb_driver_t whose probe()/remove() run the device bring-up; the module
 * drives transfers through the usb_device_t's host-controller ops.
 *
 * This mirrors fs_mod — the other non-PCI module class: the loader relocates
 * the image, resolves its undefined symbols through ksym_resolve(), and keeps
 * it resident in a slot under its instance name. */

#define USB_MOD_MAX  8

/* Non-destructive probe: does the module at 'path' export 'usb_driver_init'?
 * Returns 1 if it does, 0 if not, negative on read/validation error. */
int  usb_mod_detect(const char *path);

/* Load a USB driver module from the staged cctkfs image and call its
 * usb_driver_init().  Returns 0 on success, negative errno otherwise. */
int  usb_mod_load(const char *path);

/* Unload by instance name (module basename without ".cctk"). */
int  usb_mod_unload(const char *instance);

int  usb_mod_loaded(const char *instance);
int  usb_mod_count(void);
const char *usb_mod_instance(int slot);

#endif
