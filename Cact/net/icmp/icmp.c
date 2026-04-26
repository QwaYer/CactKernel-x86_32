#include "icmp.h"
#include "ip.h"
#include "kernel.h"
#include "memory.h"

/* ───────────────────────────────────────────────
   RX: handle incoming ICMP (only echo-request)
   ─────────────────────────────────────────────── */
void icmp_input(skb_t* skb) {
    if (skb_len(skb) < sizeof(icmp_header_t)) goto drop;

    icmp_header_t* icmp = (icmp_header_t*)skb_data(skb);
    skb->icmp = icmp;

    /* Validate ICMP checksum */
    uint16_t saved = icmp->checksum;
    icmp->checksum = 0;
    if (inet_checksum(icmp, skb_len(skb)) != saved) {
        kprint("[ICMP] Checksum error\n");
        goto drop;
    }
    icmp->checksum = saved;

    if (icmp->type == ICMP_ECHO_REPLY) {
        kprint("Reply from ");
        char buf[16];
        uint32_t src = ntohl(skb->ip->src_ip);
        itoa((src >> 24) & 0xFF, buf); kprint(buf); kprint(".");
        itoa((src >> 16) & 0xFF, buf); kprint(buf); kprint(".");
        itoa((src >> 8) & 0xFF, buf); kprint(buf); kprint(".");
        itoa(src & 0xFF, buf); kprint(buf);
        kprint(": bytes=32\n");
        goto drop;
    }

    if (icmp->type != ICMP_ECHO_REQUEST) goto drop;  /* only handle ping */

    /* Build reply — reuse the incoming skb's payload as the reply body */
    uint16_t payload_len = skb_len(skb);  /* icmp header + data */
    uint32_t src_ip      = skb->ip->src_ip;

    skb_t* reply = skb_alloc();
    if (!reply) goto drop;

    /* Copy ICMP header + payload into reply */
    uint8_t* body = skb_put(reply, payload_len);
    icmp_header_t* r = (icmp_header_t*)body;
    /* Copy original data (id, seq, payload) */
    for (uint16_t i = 0; i < payload_len; i++)
        body[i] = ((uint8_t*)icmp)[i];

    r->type     = ICMP_ECHO_REPLY;
    r->code     = 0;
    r->checksum = 0;
    r->checksum = inet_checksum(r, payload_len);

    ip_output(reply, src_ip, IP_PROTO_ICMP);
    skb_free(reply);

drop:
    skb_free(skb);
}

/* ───────────────────────────────────────────────
   TX: send an ICMP echo request (ping)
   ─────────────────────────────────────────────── */
void icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq) {
    static const char payload[] = "CactOS ping!";
    uint16_t payload_len = sizeof(payload) - 1;
    uint16_t total       = sizeof(icmp_header_t) + payload_len;

    skb_t* skb = skb_alloc();
    if (!skb) return;

    icmp_header_t* icmp = (icmp_header_t*)skb_put(skb, total);
    icmp->type     = ICMP_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(id);
    icmp->seq      = htons(seq);

    uint8_t* data = (uint8_t*)(icmp + 1);
    for (uint16_t i = 0; i < payload_len; i++) data[i] = payload[i];

    icmp->checksum = inet_checksum(icmp, total);

        ip_output(skb, dst_ip, IP_PROTO_ICMP);
}
