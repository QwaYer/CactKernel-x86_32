#include "arp.h"
#include "ethernet.h"
#include "kernel.h"
#include "memory.h"

#define ARP_CACHE_SIZE 16

typedef struct {
    uint32_t   ip;
    mac_addr_t mac;
    int        valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

void arp_init(void) {
    kprint("[ARP] clearing cache (16 slots, ip+mac+valid each)\n");
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = 0;
    }
    klog(LOG_OK, "ARP cache ready — 16 slots zeroed");
}

void arp_input(skb_t* skb) {
    if (skb_len(skb) < sizeof(arp_header_t)) {
        skb_free(skb);
        return;
    }

    arp_header_t* ah = (arp_header_t*)skb_data(skb);
    skb->arp = ah;

    uint16_t oper = ntohs(ah->oper);
    uint32_t spa = ntohl(ah->spa);
    uint32_t tpa = ntohl(ah->tpa);

    int found = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == spa) {
            arp_cache[i].mac = ah->sha;
            found = 1;
            break;
        }
    }
    if (!found) {
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (!arp_cache[i].valid) {
                arp_cache[i].ip = spa;
                arp_cache[i].mac = ah->sha;
                arp_cache[i].valid = 1;
                break;
            }
        }
    }

    if (oper == ARP_OP_REQUEST && tpa == MY_IP) {
        skb_t* reply = skb_alloc();
        if (!reply) {
            skb_free(skb);
            return;
        }

        arp_header_t* rah = (arp_header_t*)skb_push(reply, sizeof(arp_header_t));
        rah->htype = htons(ARP_HTYPE_ETH);
        rah->ptype = htons(ARP_PTYPE_IP);
        rah->hlen  = 6;
        rah->plen  = 4;
        rah->oper  = htons(ARP_OP_REPLY);
        rah->sha   = my_mac;
        rah->spa   = htonl(MY_IP);
        rah->tha   = ah->sha;
        rah->tpa   = ah->spa;

        ethernet_output(reply, ah->sha, ETH_TYPE_ARP);
        skb_free(reply);
    }

    skb_free(skb);
}

void arp_request(uint32_t target_ip) {
    skb_t* skb = skb_alloc();
    if (!skb) return;

    arp_header_t* ah = (arp_header_t*)skb_push(skb, sizeof(arp_header_t));
    ah->htype = htons(ARP_HTYPE_ETH);
    ah->ptype = htons(ARP_PTYPE_IP);
    ah->hlen  = 6;
    ah->plen  = 4;
    ah->oper  = htons(ARP_OP_REQUEST);
    ah->sha   = my_mac;
    ah->spa   = htonl(MY_IP);
    ah->tha   = (mac_addr_t){{0,0,0,0,0,0}};
    ah->tpa   = target_ip; 

        ethernet_output(skb, MAC_BROADCAST, ETH_TYPE_ARP);
        skb_free(skb);
}
int arp_lookup(uint32_t ip, mac_addr_t* out_mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            *out_mac = arp_cache[i].mac;
            return 1;
        }
    }
    return 0;
}
