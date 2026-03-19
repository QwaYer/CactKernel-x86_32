#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stddef.h>


#define USB_SPEED_LOW   0
#define USB_SPEED_FULL  1
#define USB_SPEED_HIGH  2


#define USB_TRANSFER_CONTROL     0
#define USB_TRANSFER_ISOCHRONOUS 1
#define USB_TRANSFER_BULK        2
#define USB_TRANSFER_INTERRUPT   3


#define USB_DIR_OUT  0
#define USB_DIR_IN   1


#define USB_RT_HOST_TO_DEV  0x00
#define USB_RT_DEV_TO_HOST  0x80
#define USB_RT_CLASS        0x20
#define USB_RT_VENDOR       0x40
#define USB_RT_INTERFACE    0x01
#define USB_RT_OTHER        0x03


#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B


#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HUB            0x29


#define USB_CLASS_HID    0x03
#define USB_CLASS_HUB    0x09
#define USB_CLASS_MASS   0x08
#define USB_CLASS_VENDOR 0xFF


#define USB_HID_SUBCLASS_BOOT   0x01
#define USB_HID_PROTOCOL_KBD    0x01
#define USB_HID_PROTOCOL_MOUSE  0x02


#define USB_MAX_DEVICES   32
#define USB_MAX_HCS        4
#define USB_MAX_PORTS      8
#define USB_MAX_ENDPOINTS  8
#define USB_MAX_DRIVERS   16
#define USB_DEFAULT_ADDRESS 0


typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_pkt_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_dev_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_cfg_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_iface_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_ep_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
    uint8_t  DeviceRemovable[2];
    uint8_t  PortPwrCtrlMask[2];
} __attribute__((packed)) usb_hub_desc_t;

typedef struct {
    uint8_t  address;
    uint8_t  direction;
    uint8_t  transfer_type;
    uint16_t max_packet;
    uint8_t  interval;
    uint8_t  toggle;
} usb_endpoint_t;

struct usb_hc;
struct usb_device;

typedef struct usb_device {
    uint8_t          address;
    uint8_t          speed;
    uint8_t          port;
    struct usb_device *hub;
    struct usb_hc    *hc;

    usb_dev_desc_t   dev_desc;
    uint8_t          config_buf[256];
    uint16_t         config_len;

    usb_endpoint_t   ep[USB_MAX_ENDPOINTS];
    uint8_t          ep_count;

    uint8_t          class_code;
    uint8_t          subclass;
    uint8_t          protocol;

    void            *driver_priv;

    struct usb_device *next;
} usb_device_t;

typedef void (*usb_irq_notify_fn_t)(struct usb_device *dev,
                                     void *buf, uint16_t len,
                                     void *priv);

typedef struct usb_hc {
    const char *name;
    uint8_t     hc_id;

    int  (*control_transfer)  (struct usb_hc *hc, usb_device_t *dev,
                                usb_setup_pkt_t *setup,
                                void *data, uint16_t len);
    int  (*interrupt_transfer)(struct usb_hc *hc, usb_device_t *dev,
                                uint8_t ep, void *buf, uint16_t len);
    int  (*bulk_transfer)     (struct usb_hc *hc, usb_device_t *dev,
                                uint8_t ep, uint8_t dir,
                                void *buf, uint16_t len);
    int  (*port_reset)        (struct usb_hc *hc, uint8_t port);
    int  (*port_get_status)   (struct usb_hc *hc, uint8_t port);

    uint8_t  num_ports;
    void    *priv;

    struct usb_hc *irq_next;

    struct usb_hc *next;
} usb_hc_t;

typedef struct usb_driver {
    const char *name;
    uint8_t     class_code;
    uint8_t     subclass;
    uint8_t     protocol;

    int  (*probe) (usb_device_t *dev);
    void (*remove)(usb_device_t *dev);

    struct usb_driver *next;
} usb_driver_t;


//public api
void usb_init(void);

int  usb_hc_register    (usb_hc_t    *hc);
int  usb_driver_register(usb_driver_t *drv);

usb_device_t *usb_device_enumerate (usb_hc_t *hc, uint8_t port, uint8_t speed);
void          usb_device_disconnect(usb_hc_t *hc, uint8_t port);
int           usb_register_device  (usb_device_t *dev);

int usb_get_descriptor    (usb_device_t *dev, uint8_t type, uint8_t idx,
                            void *buf, uint16_t len);
int usb_set_address       (usb_device_t *dev, uint8_t addr);
int usb_set_configuration (usb_device_t *dev, uint8_t cfg);
int usb_set_interface     (usb_device_t *dev, uint8_t iface, uint8_t alt);

void usb_dump_devices(void);

uint8_t usb_alloc_address(void);
void    usb_free_address (uint8_t addr);

#endif