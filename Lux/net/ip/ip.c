#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "arp.h"
#include "ethernet.h"
#include "kernel.h"

static uint16_t ip_id_counter = 1;

/* ───────────────────────────────────────────────
   RX: validate IP header, demux to upper layer
   ─────────────────────────────────────────────── */
void ip_input(skb_t* skb) {
    if (skb_len(skb) < sizeof(ip_header_t)) goto drop;

    ip_header_t* iph = (ip_header_t*)skb_data(skb);
    skb->ip = iph;

    /* Basic sanity checks */
    if (IP_VERSION(iph) != 4)        goto drop;
    uint16_t ihl = IP_HDR_LEN(iph);
    if (ihl < 20)                    goto drop;
    if (ntohs(iph->total_len) < ihl) goto drop;

    /* Verify header checksum */
    uint16_t saved = iph->checksum;
    iph->checksum  = 0;
    if (inet_checksum(iph, ihl) != saved) goto drop;
    iph->checksum  = saved;

    /* Only accept packets addressed to us (or broadcast) */
    uint32_t dst = ntohl(iph->dst_ip);
    if (dst != MY_IP && dst != 0xFFFFFFFF) goto drop;

    /* Advance past IP header */
    skb->data_offset += ihl;
    skb->total_len   -= ihl;

    switch (iph->protocol) {
        case IP_PROTO_ICMP: icmp_input(skb); break;
        case IP_PROTO_UDP:  udp_input(skb);  break;
        case IP_PROTO_TCP:  tcp_input(skb);  break;
        default:            goto drop;
    }
    return;

drop:
    skb_free(skb);
}

/* ───────────────────────────────────────────────
   TX: prepend IP header, resolve next-hop via ARP
   ─────────────────────────────────────────────── */
int ip_output(skb_t* skb, uint32_t dst_ip, uint8_t protocol) {
    uint16_t payload_len = skb_len(skb);

    /* Prepend IP header */
    ip_header_t* iph = (ip_header_t*)skb_push(skb, sizeof(ip_header_t));
    if (!iph) return -1;

    iph->version_ihl = (4 << 4) | 5;   /* IPv4, 20-byte header */
    iph->tos         = 0;
    iph->total_len   = htons(sizeof(ip_header_t) + payload_len);
    iph->id          = htons(ip_id_counter++);
    iph->flags_frag  = htons(0x4000);  /* Don't Fragment */
    iph->ttl         = 64;
    iph->protocol    = protocol;
    iph->checksum    = 0;
    iph->src_ip      = htonl(MY_IP);
    iph->dst_ip      = dst_ip;         /* caller passes in network order */

    /* Compute IP header checksum */
    iph->checksum = inet_checksum(iph, sizeof(ip_header_t));

    /* ── Routing: simple — everything goes to gateway unless on-link ── */
    uint32_t next_hop;
    uint32_t dst_host = ntohl(dst_ip);
    if ((dst_host & MY_NETMASK) == (MY_IP & MY_NETMASK)) {
        next_hop = dst_ip;              /* on-link: ARP the destination  */
    } else {
        next_hop = htonl(MY_GATEWAY);  /* off-link: ARP the gateway     */
    }

    /* ARP resolve next hop */
    mac_addr_t next_mac;
    if (!arp_lookup(ntohl(next_hop), &next_mac)) {
        /* Cache miss — send ARP request.
           Packet is dropped; upper layer should retry (simple for now). */
        arp_request(next_hop);
        skb_free(skb);
        return -2;   /* -2 = "ARP pending, try again later" */
    }

    int res = ethernet_output(skb, next_mac, ETH_TYPE_IPV4);
    skb_free(skb);
    return res;
}

/* ───────────────────────────────────────────────
   Pseudo-header checksum for TCP / UDP
   ─────────────────────────────────────────────── */
uint16_t ip_pseudo_checksum(uint32_t src, uint32_t dst,
                            uint8_t proto, uint16_t len) {
    struct {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t len;
    } __attribute__((packed)) pseudo = { src, dst, 0, proto, htons(len) };
    return inet_checksum(&pseudo, sizeof(pseudo));
}