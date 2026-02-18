#include "udp.h"
#include "ip.h"
#include "net.h"
#include "kernel.h"

void udp_input(skb_t* skb) {
    if (skb_len(skb) < sizeof(udp_header_t)) {
        skb_free(skb);
        return;
    }

    udp_header_t* udph = (udp_header_t*)skb_data(skb);
    skb->udp = udph;

    uint16_t src_port = ntohs(udph->src_port);
    uint16_t dst_port = ntohs(udph->dst_port);
    uint16_t len = ntohs(udph->length);

    skb->data_offset += sizeof(udp_header_t);
    skb->total_len   -= sizeof(udp_header_t);

    skb_free(skb);
}

int udp_output(skb_t* skb, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
    uint16_t payload_len = skb_len(skb);
    
    udp_header_t* udph = (udp_header_t*)skb_push(skb, sizeof(udp_header_t));
    if (!udph) return -1;

    udph->src_port = htons(src_port);
    udph->dst_port = htons(dst_port);
    udph->length   = htons(sizeof(udp_header_t) + payload_len);
    udph->checksum = 0;
    
    return ip_output(skb, dst_ip, IP_PROTO_UDP);
}
