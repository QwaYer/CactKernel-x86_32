#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include "net.h"

/* ───────────────────────────────────────────────
   TCP Header
   ─────────────────────────────────────────────── */
typedef struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   /* top 4 bits: header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
    /* Options follow if data_offset > 5 */
} __attribute__((packed)) tcp_header_t;

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

/* TCP connection states */
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

/* ───────────────────────────────────────────────
   TCP Socket (PCB — Protocol Control Block)
   ─────────────────────────────────────────────── */
#define TCP_MAX_SOCKETS  8
#define TCP_RX_BUF_SIZE  4096
#define TCP_TX_BUF_SIZE  4096

typedef void (*tcp_data_fn)(int sock_id, uint8_t* data, uint16_t len);
typedef void (*tcp_event_fn)(int sock_id, tcp_state_t new_state);

typedef struct tcp_socket {
    uint8_t     used;
    tcp_state_t state;

    uint32_t    local_ip;
    uint16_t    local_port;
    uint32_t    remote_ip;
    uint16_t    remote_port;

    uint32_t    snd_una;    /* oldest unacknowledged sequence number */
    uint32_t    snd_nxt;    /* next sequence number to send          */
    uint32_t    snd_wnd;    /* send window (from peer)               */

    uint32_t    rcv_nxt;    /* next expected sequence number         */
    uint32_t    rcv_wnd;    /* our receive window (advertise to peer)*/

    /* Simple linear RX buffer (ring buffer upgrade later) */
    uint8_t     rx_buf[TCP_RX_BUF_SIZE];
    uint16_t    rx_head, rx_tail;

    tcp_data_fn  on_data;
    tcp_event_fn on_event;

    /* Accept support */
    int8_t  listen_parent;  /* index of parent LISTEN socket, or -1 */
    uint8_t accept_ready;   /* 1 when this connection is ready for accept() */
} tcp_socket_t;

/* ───────────────────────────────────────────────
   Public API — skeleton stubs
   ─────────────────────────────────────────────── */

extern tcp_socket_t tcp_sockets[TCP_MAX_SOCKETS];

/* Create a socket, returns socket id or -1 */
int  tcp_socket(void);

/* Active open */
int  tcp_connect(int sock, uint32_t dst_ip, uint16_t dst_port);

/* Passive open (server) */
int  tcp_listen(int sock, uint16_t local_port);

/* Send data on an established connection */
int  tcp_send(int sock, uint8_t* data, uint16_t len);

/* Close (sends FIN) */
int  tcp_close(int sock);

/* Register callbacks */
void tcp_set_callbacks(int sock, tcp_data_fn on_data, tcp_event_fn on_event);

/* Read buffered RX data; returns bytes copied, 0 if nothing available */
int tcp_recv(int sock, uint8_t* buf, uint16_t max_len);

/* Called by ip_input() */
void tcp_input(skb_t* skb);

#endif /* TCP_H */