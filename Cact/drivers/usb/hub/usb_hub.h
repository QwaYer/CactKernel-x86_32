#ifndef USB_HUB_H
#define USB_HUB_H

#include <stdint.h>
#include "usb.h"


#define HUB_REQ_GET_STATUS      0x00
#define HUB_REQ_CLEAR_FEATURE   0x01
#define HUB_REQ_SET_FEATURE     0x03
#define HUB_REQ_GET_DESCRIPTOR  0x06


#define PORT_CONNECTION    0
#define PORT_ENABLE        1
#define PORT_SUSPEND       2
#define PORT_OVER_CURRENT  3
#define PORT_RESET         4
#define PORT_POWER         8
#define PORT_LOW_SPEED     9
#define C_PORT_CONNECTION  16
#define C_PORT_ENABLE      17
#define C_PORT_SUSPEND     18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET       20


#define HUB_PORT_STS_CONNECTION  0x0001
#define HUB_PORT_STS_ENABLE      0x0002
#define HUB_PORT_STS_RESET       0x0010
#define HUB_PORT_STS_POWER       0x0100
#define HUB_PORT_STS_LOW_SPEED   0x0200
#define HUB_PORT_STS_HIGH_SPEED  0x0400

#define HUB_PORT_CHG_CONNECTION  0x0001
#define HUB_PORT_CHG_RESET       0x0010

typedef struct {
    uint16_t wPortStatus;
    uint16_t wPortChange;
} __attribute__((packed)) hub_port_status_t;


typedef struct usb_hub_priv {
    usb_device_t  *dev;
    usb_hub_desc_t desc;
    uint8_t        num_ports;
    uint8_t        intr_ep;
    uint8_t        status_buf[8];
    volatile int   removed;
    /* Ports whose change bitmap the hub reported from interrupt context.  The
     * reset/enumeration they need happens in usb_hub_task(), not in the
     * notify callback (which runs with the controller's event lock held). */
    volatile uint32_t port_events;
    struct usb_hub_priv *next;
} usb_hub_priv_t;

//Public api
void usb_hub_init(void);
/* Spawn the task that services hub port changes (needs the scheduler). */
void usb_hub_hotplug_init(void);

#endif