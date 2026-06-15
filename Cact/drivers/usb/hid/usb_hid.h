#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include "usb.h"


#define HID_REQ_GET_REPORT   0x01
#define HID_REQ_GET_IDLE     0x02
#define HID_REQ_GET_PROTOCOL 0x03
#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

#define HID_PROTO_BOOT   0x00
#define HID_PROTO_REPORT 0x01

typedef struct {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} __attribute__((packed)) hid_kbd_report_t;

#define HID_MOD_LCTRL  0x01
#define HID_MOD_LSHIFT 0x02
#define HID_MOD_LALT   0x04
#define HID_MOD_LMETA  0x08
#define HID_MOD_RCTRL  0x10
#define HID_MOD_RSHIFT 0x20
#define HID_MOD_RALT   0x40
#define HID_MOD_RMETA  0x80

typedef struct {
    uint8_t buttons;
    int8_t  x;
    int8_t  y;
    int8_t  wheel;
} __attribute__((packed)) hid_mouse_report_t;

typedef struct {
    uint8_t  buttons;
    uint16_t x;          
    uint16_t y;
    int8_t   wheel;
} __attribute__((packed)) hid_tablet_report_t;

typedef enum { HID_TYPE_KEYBOARD, HID_TYPE_MOUSE, HID_TYPE_TABLET, HID_TYPE_UNKNOWN } hid_type_t;

typedef struct {
    hid_type_t  type;
    uint8_t     intr_ep;
    uint8_t     iface_num;
    volatile uint8_t  removed;
    union {
        hid_kbd_report_t    kbd;
        hid_mouse_report_t  mouse;
        hid_tablet_report_t tablet;
    } report_buf;
    hid_kbd_report_t prev_kbd;
    uint8_t          caps_lock;
} hid_priv_t;


//Public api
void usb_hid_init(void);

extern volatile char usb_last_char;
extern volatile int  usb_key_event;
extern volatile int  usb_mouse_dx;
extern volatile int  usb_mouse_dy;
extern volatile int  usb_mouse_buttons;

#endif