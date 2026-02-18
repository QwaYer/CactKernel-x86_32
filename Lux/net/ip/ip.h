#ifndef IP_H
#define IP_H

#include <stdint.h>
#include "net.h"

/* IP protocols */
#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

typedef struct ip_header {
    uint8_t  version_ihl;      /* version (4) | IHL (header len in 32-bit words) */
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;       /* flags (3 bits) | fragment offset (13 bits)      */
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ip_header_t;

#define IP_HDR_LEN(iph)  (((iph)->version_ihl & 0x0F) * 4)
#define IP_VERSION(iph)  (((iph)->version_ihl >> 4) & 0xF)

/* RX */
void ip_input(skb_t* skb);

/* TX — route and send an IP packet.
   payload already in skb; this prepends IP header + calls ARP/ethernet. */
int ip_output(skb_t* skb, uint32_t dst_ip, uint8_t protocol);

/* Pseudo-header checksum helper (used by TCP/UDP) */
uint16_t ip_pseudo_checksum(uint32_t src, uint32_t dst,
                            uint8_t proto, uint16_t len);

#endif /* IP_H */