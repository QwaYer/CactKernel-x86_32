#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include "net.h"

typedef struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

void udp_input(skb_t* skb);
int udp_output(skb_t* skb, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port);

#endif 
