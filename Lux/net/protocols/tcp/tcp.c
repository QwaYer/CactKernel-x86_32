#include "tcp.h"
#include "ip.h"
#include "kernel.h"
#include "memory.h"

static tcp_socket_t tcp_sockets[TCP_MAX_SOCKETS];

/* ─────────────────────────────────────────────── */
/*   Socket management                             */
/* ─────────────────────────────────────────────── */
int tcp_socket(void) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!tcp_sockets[i].used) {
            tcp_sockets[i].used     = 1;
            tcp_sockets[i].state    = TCP_CLOSED;
            tcp_sockets[i].rx_head  = 0;
            tcp_sockets[i].rx_tail  = 0;
            tcp_sockets[i].snd_nxt  = 0xC0FFEE00;  /* arbitrary ISN */
            tcp_sockets[i].rcv_wnd  = TCP_RX_BUF_SIZE;
            return i;
        }
    }
    return -1;
}

void tcp_set_callbacks(int sock, tcp_data_fn on_data, tcp_event_fn on_event) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return;
    tcp_sockets[sock].on_data  = on_data;
    tcp_sockets[sock].on_event = on_event;
}

/* ─────────────────────────────────────────────── */
/*   Low-level TX helper                           */
/* ─────────────────────────────────────────────── */
static int tcp_send_segment(tcp_socket_t* s, uint8_t flags,
                             uint8_t* data, uint16_t data_len) {
    skb_t* skb = skb_alloc();
    if (!skb) return -1;

    /* Payload */
    if (data && data_len) {
        uint8_t* p = skb_put(skb, data_len);
        for (uint16_t i = 0; i < data_len; i++) p[i] = data[i];
    }

    /* TCP header */
    tcp_header_t* tcp = (tcp_header_t*)skb_push(skb, sizeof(tcp_header_t));
    tcp->src_port   = htons(s->local_port);
    tcp->dst_port   = htons(s->remote_port);
    tcp->seq_num    = htonl(s->snd_nxt);
    tcp->ack_num    = (flags & TCP_ACK) ? htonl(s->rcv_nxt) : 0;
    tcp->data_offset= (5 << 4);   /* 20-byte header, no options */
    tcp->flags      = flags;
    tcp->window     = htons(s->rcv_wnd);
    tcp->urgent_ptr = 0;
    tcp->checksum   = 0;

    /* TCP checksum uses IP pseudo-header */
    uint16_t tcp_total = sizeof(tcp_header_t) + data_len;
    uint16_t pseudo = ip_pseudo_checksum(
        htonl(MY_IP), s->remote_ip,
        IP_PROTO_TCP, tcp_total
    );
    /* Add TCP segment on top of pseudo checksum */
    uint32_t sum = (uint32_t)(~pseudo) + (uint32_t)inet_checksum(tcp, tcp_total);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    tcp->checksum = (uint16_t)(~sum);

    if (flags & (TCP_SYN | TCP_FIN)) s->snd_nxt++;
    s->snd_nxt += data_len;

    int ret = ip_output(skb, s->remote_ip, IP_PROTO_TCP);
    skb_free(skb);
    return ret;
}

/* ─────────────────────────────────────────────── */
/*   Active open (client)                          */
/* ─────────────────────────────────────────────── */
int tcp_connect(int sock, uint32_t dst_ip, uint16_t dst_port) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return -1;
    tcp_socket_t* s = &tcp_sockets[sock];
    if (s->state != TCP_CLOSED) return -1;

    s->remote_ip   = dst_ip;
    s->remote_port = dst_port;
    s->local_ip    = htonl(MY_IP);
    /* Pick an ephemeral port */
    static uint16_t next_port = 49152;
    s->local_port = next_port++;

    s->state = TCP_SYN_SENT;
    return tcp_send_segment(s, TCP_SYN, NULL, 0);
}

/* ─────────────────────────────────────────────── */
/*   Passive open (server)                         */
/* ─────────────────────────────────────────────── */
int tcp_listen(int sock, uint16_t local_port) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return -1;
    tcp_socket_t* s = &tcp_sockets[sock];
    if (s->state != TCP_CLOSED) return -1;
    s->local_port = local_port;
    s->state      = TCP_LISTEN;
    return 0;
}

/* ─────────────────────────────────────────────── */
/*   Send data                                     */
/* ─────────────────────────────────────────────── */
int tcp_send(int sock, uint8_t* data, uint16_t len) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return -1;
    tcp_socket_t* s = &tcp_sockets[sock];
    if (s->state != TCP_ESTABLISHED) return -1;
    return tcp_send_segment(s, TCP_ACK | TCP_PSH, data, len);
}

/* ─────────────────────────────────────────────── */
/*   Close                                         */
/* ─────────────────────────────────────────────── */
int tcp_close(int sock) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return -1;
    tcp_socket_t* s = &tcp_sockets[sock];
    if (s->state == TCP_ESTABLISHED) {
        s->state = TCP_FIN_WAIT_1;
        return tcp_send_segment(s, TCP_FIN | TCP_ACK, NULL, 0);
    }
    s->state = TCP_CLOSED;
    s->used  = 0;
    return 0;
}

/* ─────────────────────────────────────────────── */
/*   RX: state machine                             */
/* ─────────────────────────────────────────────── */
void tcp_input(skb_t* skb) {
    if (skb_len(skb) < sizeof(tcp_header_t)) goto drop;

    tcp_header_t* tcp = (tcp_header_t*)skb_data(skb);
    skb->tcp = tcp;

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq      = ntohl(tcp->seq_num);
    uint32_t ack      = ntohl(tcp->ack_num);
    uint8_t  flags    = tcp->flags;
    uint16_t hdr_len  = ((tcp->data_offset >> 4) & 0xF) * 4;

    if (hdr_len < sizeof(tcp_header_t)) goto drop;

    uint8_t*  payload     = (uint8_t*)tcp + hdr_len;
    uint16_t  payload_len = skb_len(skb) > hdr_len
                            ? skb_len(skb) - hdr_len : 0;

    /* Find matching socket */
    tcp_socket_t* s = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t* c = &tcp_sockets[i];
        if (!c->used) continue;

        if (c->state == TCP_LISTEN && c->local_port == dst_port) {
            /* Accept new connection: fill in remote info */
            c->remote_ip   = skb->ip->src_ip;
            c->remote_port = src_port;
            s = c; break;
        }
        if (c->local_port  == dst_port &&
            c->remote_port == src_port &&
            c->remote_ip   == skb->ip->src_ip) {
            s = c; break;
        }
    }

    if (!s) {
        /* Send RST for unknown connection */
        /* TODO: build a RST skb */
        goto drop;
    }

    /* ── Mini state machine ── */
    switch (s->state) {

    case TCP_LISTEN:
        if (flags & TCP_SYN) {
            s->rcv_nxt  = seq + 1;
            s->snd_una  = s->snd_nxt;
            s->state    = TCP_SYN_RECEIVED;
            /* Send SYN-ACK */
            tcp_send_segment(s, TCP_SYN | TCP_ACK, NULL, 0);
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            s->rcv_nxt  = seq + 1;
            s->snd_una  = ack;
            s->state    = TCP_ESTABLISHED;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            if (s->on_event) s->on_event((int)(s - tcp_sockets), TCP_ESTABLISHED);
        }
        break;

    case TCP_SYN_RECEIVED:
        if ((flags & TCP_ACK) && ack == s->snd_nxt) {
            s->snd_una = ack;
            s->state   = TCP_ESTABLISHED;
            if (s->on_event) s->on_event((int)(s - tcp_sockets), TCP_ESTABLISHED);
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) { s->state = TCP_CLOSED; s->used = 0; break; }

        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            s->state = TCP_CLOSE_WAIT;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            if (s->on_event) s->on_event((int)(s - tcp_sockets), TCP_CLOSE_WAIT);
            break;
        }

        if (payload_len && seq == s->rcv_nxt) {
            s->rcv_nxt += payload_len;
            /* Copy into RX buffer */
            for (uint16_t i = 0; i < payload_len; i++) {
                s->rx_buf[s->rx_tail % TCP_RX_BUF_SIZE] = payload[i];
                s->rx_tail++;
            }
            if (s->on_data) s->on_data((int)(s - tcp_sockets), payload, payload_len);
            /* ACK the data */
            tcp_send_segment(s, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_ACK) { s->state = TCP_FIN_WAIT_2; }
        break;

    case TCP_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            s->rcv_nxt++;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            s->state = TCP_TIME_WAIT;
            /* Ideally start a 2*MSL timer; for now just close */
            s->state = TCP_CLOSED;
            s->used  = 0;
        }
        break;

    case TCP_CLOSE_WAIT:
        /* Application should call tcp_close() to send FIN */
        break;

    default:
        break;
    }

drop:
    skb_free(skb);
}