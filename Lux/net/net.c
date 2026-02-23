#include "net.h"
#include "ethernet/ethernet.h"
#include "kernel.h"   
#include "memory.h"

net_driver_t* active_nic = NULL;
mac_addr_t    my_mac;
semaphore_t   net_sema;

skb_t* skb_alloc(void) {
    skb_t* skb = (skb_t*)kmalloc(sizeof(skb_t));
    if (!skb) return NULL;

    skb->data_offset = SKB_MAX_SIZE / 2;
    skb->total_len   = 0;
    skb->eth = NULL; skb->arp = NULL; skb->ip  = NULL;
    skb->icmp= NULL; skb->udp = NULL; skb->tcp = NULL;
    return skb;
}

void skb_free(skb_t* skb) {
    if (skb) kfree_heap(skb);
}

uint8_t* skb_push(skb_t* skb, uint16_t len) {
    skb->data_offset -= len;
    skb->total_len   += len;
    return skb->data + skb->data_offset;
}

uint8_t* skb_put(skb_t* skb, uint16_t len) {
    uint8_t* ptr = skb->data + skb->data_offset + skb->total_len;
    skb->total_len += len;
    return ptr;
}

uint8_t* skb_data(skb_t* skb) { return skb->data + skb->data_offset; }
uint16_t skb_len (skb_t* skb) { return skb->total_len; }

uint16_t inet_checksum(void* data, uint16_t len) {
    uint32_t sum = 0;
    uint16_t* ptr = (uint16_t*)data;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len) sum += *(uint8_t*)ptr;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

void net_register_driver(net_driver_t* drv) {
    active_nic = drv;
    drv->get_mac(&my_mac);
    my_mac = drv->mac;
}

static void knetd() {
    while (1) {
        sema_down(&net_sema);
        net_poll();
    }
}

void net_init(void) {
    sema_init(&net_sema, 0);
    create_task(knetd);

    /* 1. Bring up the NIC driver */
    extern void virtio_net_init(void);
    virtio_net_init();

    /* 2. Upper-layer init (ARP cache, routing table …) */
    extern void arp_init(void);
    arp_init();
}

void net_receive(skb_t* skb) {
    if (!skb) return;
    ethernet_input(skb);   
}

/* ───────────────────────────────────────────────
   Polling — call from scheduler tick or idle loop
   ─────────────────────────────────────────────── */
void net_poll(void) {
    if (active_nic && active_nic->poll)
        active_nic->poll();
    
    /* Handle ARP timeouts or other periodic tasks if needed */
}
