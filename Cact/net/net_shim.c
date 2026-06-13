#include "net.h"
#include "sync.h"

void net_receive_packet(skb_t* skb) {
    net_receive(skb);
}

void net_driver_irq_wake(void) {
    sema_up(&net_sema);
}
