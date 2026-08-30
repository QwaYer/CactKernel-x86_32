#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include "sync.h"

/* ───────────────────────────────────────────────
   Compile-time network configuration
   ─────────────────────────────────────────────── */
#define NET_IP_ADDR(a,b,c,d)  (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

/* Change these to match your QEMU TAP setup */
#define MY_IP       NET_IP_ADDR(10,0,2,15)
#define MY_NETMASK  NET_IP_ADDR(255,255,255,0)
#define MY_GATEWAY  NET_IP_ADDR(10,0,2,2)

/* ───────────────────────────────────────────────
   MAC address
   ─────────────────────────────────────────────── */
typedef struct {
    uint8_t b[6];
} __attribute__((packed)) mac_addr_t;

extern mac_addr_t my_mac;

/* Broadcast MAC */
#define MAC_BROADCAST ((mac_addr_t){{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}})

static inline int mac_equal(mac_addr_t a, mac_addr_t b) {
    for (int i = 0; i < 6; i++) if (a.b[i] != b.b[i]) return 0;
    return 1;
}

#define SKB_MAX_SIZE 2048

typedef struct sk_buff {
    uint8_t  data[SKB_MAX_SIZE];  /* raw frame bytes                  */
    uint16_t total_len;           /* total bytes in data[]            */
    uint16_t data_offset;         /* start of "current layer" payload */

    struct eth_header*  eth;
    struct arp_header*  arp;
    struct ip_header*   ip;
    struct icmp_header* icmp;
    struct udp_header*  udp;
    struct tcp_header*  tcp;
} skb_t;

skb_t* skb_alloc(void);
void   kfree_skb(skb_t* skb);

/* Reserve headroom so lower layers can prepend their header */
uint8_t* skb_push(skb_t* skb, uint16_t len);
uint8_t* skb_put (skb_t* skb, uint16_t len);
uint8_t* skb_data(skb_t* skb);
uint16_t skb_len (skb_t* skb);

static inline uint16_t htons(uint16_t x) { return (x>>8)|(x<<8); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x>>24)&0xFF)|((x>>8)&0xFF00)|((x<<8)&0xFF0000)|((x<<24)&0xFF000000);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Generic internet checksum (IP / ICMP / TCP / UDP) */
uint16_t inet_checksum(void* data, uint16_t len);

typedef struct net_driver {
    mac_addr_t mac;
    int  (*send)(skb_t* skb);          /* transmit raw frame           */
    void (*poll)(void);                /* poll for received frames     */
    void (*get_mac)(mac_addr_t* out);  /* read MAC from hardware       */
    const char* name;
} net_driver_t;

extern net_driver_t* active_nic;

void register_netdev(net_driver_t* drv);
void unregister_netdev(net_driver_t* drv);
void net_init(void);                  /* called from kernel_setup_hardware() */
void net_poll(void);                  /* call periodically from scheduler    */
extern semaphore_t net_sema;
void netif_rx(skb_t* skb);        /* drivers call this on RX             */

/* Stable alias for loadable modules / external driver sources */
void net_receive_packet(skb_t* skb);

/* Wake net_poll_task after NIC IRQ (semaphore lives in Rust net core). */
void net_driver_irq_wake(void);

#endif
