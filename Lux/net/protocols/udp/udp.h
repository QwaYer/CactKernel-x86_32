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

/* ── UDP socket table ─────────────────────────────────────────────────────── */

#define UDP_SOCK_MAX      8
#define UDP_RX_BUF_SIZE  4096

typedef struct udp_sock {
    uint8_t  used;
    uint16_t local_port;
    uint32_t local_ip;    /* host byte order; 0 = INADDR_ANY */

    /* Single-datagram receive buffer */
    uint8_t  rx_buf[UDP_RX_BUF_SIZE];
    uint16_t rx_len;
    uint8_t  rx_ready;    /* 1 = a datagram is waiting */

    uint32_t last_src_ip;
    uint16_t last_src_port;
} udp_sock_t;

extern udp_sock_t udp_socks[UDP_SOCK_MAX];

/* Allocate / free a UDP socket slot */
int         udp_sock_alloc(void);
void        udp_sock_free(int idx);

/* Find a bound UDP socket by destination port */
udp_sock_t *udp_sock_find_by_port(uint16_t port);

/* Receive buffered datagram; returns bytes copied, 0 if nothing available */
int udp_sock_recv(int idx, uint8_t *buf, uint16_t max_len,
                  uint32_t *src_ip_out, uint16_t *src_port_out);

/* Send a datagram from a bound socket */
int udp_sock_send(int idx, uint32_t dst_ip, uint16_t dst_port,
                  const uint8_t *data, uint16_t len);

/* ── Raw protocol API ─────────────────────────────────────────────────────── */
void udp_input(skb_t* skb);
int  udp_output(skb_t* skb, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port);

#endif
