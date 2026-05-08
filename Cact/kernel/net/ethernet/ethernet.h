#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>
#include "net.h"

/* Ethernet EtherType values */
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IPV6   0x86DD

typedef struct eth_header {
    mac_addr_t dst;
    mac_addr_t src;
    uint16_t   ethertype;   /* in network byte order */
} __attribute__((packed)) eth_header_t;

#define ETH_HEADER_LEN  sizeof(eth_header_t)
#define ETH_MIN_PAYLOAD 46
#define ETH_MAX_PAYLOAD 1500

/* RX: called by net_receive() */
void ethernet_input(skb_t* skb);

/* TX: wrap skb with Ethernet header and send via active NIC */
int ethernet_output(skb_t* skb, mac_addr_t dst, uint16_t ethertype);

#endif /* ETHERNET_H */
