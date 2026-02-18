#include "ethernet.h"
#include "arp.h"
#include "ip.h"
#include "kernel.h"

/* ───────────────────────────────────────────────
   RX: demux by EtherType
   ─────────────────────────────────────────────── */
void ethernet_input(skb_t* skb) {
    if (skb_len(skb) < ETH_HEADER_LEN) {
        skb_free(skb);
        return;
    }

    /* Map the Ethernet header */
    skb->eth = (eth_header_t*)skb_data(skb);

    /* Accept frames destined for us or broadcast */
    if (!mac_equal(skb->eth->dst, my_mac) &&
        !mac_equal(skb->eth->dst, MAC_BROADCAST)) {
        skb_free(skb);
        return;
    }

    /* Advance past Ethernet header */
    skb->data_offset += ETH_HEADER_LEN;
    skb->total_len   -= ETH_HEADER_LEN;

    uint16_t type = ntohs(skb->eth->ethertype);

    switch (type) {
        case ETH_TYPE_ARP:
            arp_input(skb);
            break;
        case ETH_TYPE_IPV4:
            ip_input(skb);
            break;
        default:
            /* Unknown EtherType — drop */
            skb_free(skb);
            break;
    }
}

/* ───────────────────────────────────────────────
   TX: prepend Ethernet header and transmit
   ─────────────────────────────────────────────── */
int ethernet_output(skb_t* skb, mac_addr_t dst, uint16_t ethertype) {
    eth_header_t* hdr = (eth_header_t*)skb_push(skb, ETH_HEADER_LEN);
    if (!hdr) return -1;

    hdr->dst       = dst;
    hdr->src       = my_mac;
    hdr->ethertype = htons(ethertype);

    if (!active_nic || !active_nic->send) return -1;
    return active_nic->send(skb);
}