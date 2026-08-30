#include "net.h"
#include "sync.h"

void net_receive_packet(skb_t* skb) {
    netif_rx(skb);
}

void net_driver_irq_wake(void) {
    up(&net_sema);
}
