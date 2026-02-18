#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include "net.h"

#define ARP_HTYPE_ETH  1
#define ARP_PTYPE_IP   0x0800

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct arp_header {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    mac_addr_t sha;
    uint32_t   spa;
    mac_addr_t tha;
    uint32_t   tpa;
} __attribute__((packed)) arp_header_t;

void arp_init(void);
void arp_input(skb_t* skb);
void arp_request(uint32_t target_ip);
int  arp_lookup(uint32_t ip, mac_addr_t* out_mac);

#endif 
