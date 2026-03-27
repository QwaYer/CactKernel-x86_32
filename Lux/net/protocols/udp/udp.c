#include "udp.h"
#include "ip.h"
#include "net.h"
#include "kernel.h"
#include "memory.h"

/* ── UDP socket table ─────────────────────────────────────────────────────── */

udp_sock_t udp_socks[UDP_SOCK_MAX];

int udp_sock_alloc(void) {
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        if (!udp_socks[i].used) {
            udp_socks[i].used       = 1;
            udp_socks[i].local_port = 0;
            udp_socks[i].local_ip   = 0;
            udp_socks[i].rx_ready   = 0;
            udp_socks[i].rx_len     = 0;
            return i;
        }
    }
    return -1;
}

void udp_sock_free(int idx) {
    if (idx < 0 || idx >= UDP_SOCK_MAX) return;
    udp_socks[idx].used = 0;
}

udp_sock_t *udp_sock_find_by_port(uint16_t port) {
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        if (udp_socks[i].used && udp_socks[i].local_port == port)
            return &udp_socks[i];
    }
    return 0;
}

int udp_sock_recv(int idx, uint8_t *buf, uint16_t max_len,
                  uint32_t *src_ip_out, uint16_t *src_port_out) {
    if (idx < 0 || idx >= UDP_SOCK_MAX) return -1;
    udp_sock_t *s = &udp_socks[idx];
    if (!s->used || !s->rx_ready) return 0;

    uint16_t to_copy = (s->rx_len < max_len) ? s->rx_len : max_len;
    for (uint16_t i = 0; i < to_copy; i++)
        buf[i] = s->rx_buf[i];

    if (src_ip_out)   *src_ip_out   = s->last_src_ip;
    if (src_port_out) *src_port_out = s->last_src_port;

    s->rx_ready = 0;
    s->rx_len   = 0;
    return (int)to_copy;
}

int udp_sock_send(int idx, uint32_t dst_ip, uint16_t dst_port,
                  const uint8_t *data, uint16_t len) {
    if (idx < 0 || idx >= UDP_SOCK_MAX) return -1;
    udp_sock_t *s = &udp_socks[idx];
    if (!s->used) return -1;

    skb_t *skb = skb_alloc();
    if (!skb) return -1;

    uint8_t *payload = skb_put(skb, len);
    if (!payload) { skb_free(skb); return -1; }
    for (uint16_t i = 0; i < len; i++) payload[i] = data[i];

    int ret = udp_output(skb, dst_ip, s->local_port, dst_port);
    skb_free(skb);
    return ret;
}

/* ── Protocol receive ─────────────────────────────────────────────────────── */

void udp_input(skb_t* skb) {
    if (skb_len(skb) < sizeof(udp_header_t)) {
        skb_free(skb);
        return;
    }

    udp_header_t* udph = (udp_header_t*)skb_data(skb);
    skb->udp = udph;

    uint16_t dst_port = ntohs(udph->dst_port);
    uint16_t src_port = ntohs(udph->src_port);
    uint16_t udp_len  = ntohs(udph->length);
    uint32_t src_ip   = skb->ip ? skb->ip->src_ip : 0;

    skb->data_offset += sizeof(udp_header_t);
    skb->total_len   -= sizeof(udp_header_t);

    uint16_t payload_len = (udp_len > sizeof(udp_header_t))
                         ? (uint16_t)(udp_len - sizeof(udp_header_t)) : 0;
    if (payload_len > skb_len(skb))
        payload_len = (uint16_t)skb_len(skb);

    /* Deliver to a bound socket if one exists */
    udp_sock_t *sock = udp_sock_find_by_port(dst_port);
    if (sock) {
        uint16_t to_store = (payload_len < UDP_RX_BUF_SIZE)
                          ? payload_len : (uint16_t)(UDP_RX_BUF_SIZE - 1);
        uint8_t *payload = (uint8_t*)skb_data(skb);
        for (uint16_t i = 0; i < to_store; i++)
            sock->rx_buf[i] = payload[i];
        sock->rx_len       = to_store;
        sock->rx_ready     = 1;
        sock->last_src_ip  = ntohl(src_ip);
        sock->last_src_port= src_port;
    }

    skb_free(skb);
}

/* ── Protocol transmit ────────────────────────────────────────────────────── */

int udp_output(skb_t* skb, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
    uint16_t payload_len = skb_len(skb);

    udp_header_t* udph = (udp_header_t*)skb_push(skb, sizeof(udp_header_t));
    if (!udph) return -1;

    udph->src_port = htons(src_port);
    udph->dst_port = htons(dst_port);
    udph->length   = htons((uint16_t)(sizeof(udp_header_t) + payload_len));
    udph->checksum = 0;

    return ip_output(skb, dst_ip, IP_PROTO_UDP);
}
