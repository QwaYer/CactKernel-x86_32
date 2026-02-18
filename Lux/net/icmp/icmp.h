#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include "net.h"

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

typedef struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    /* Payload follows */
} __attribute__((packed)) icmp_header_t;

void icmp_input(skb_t* skb);
void icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq);

#endif /* ICMP_H */