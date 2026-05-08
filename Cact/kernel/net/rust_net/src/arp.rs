use crate::config;
use crate::ethernet;
use crate::runtime;
use crate::skb;
use crate::types::{ArpHeader, MacAddr, Skb, ARP_CACHE_SIZE, ETH_TYPE_ARP};

#[derive(Clone, Copy)]
struct ArpEntry {
    ip: u32,
    mac: MacAddr,
    valid: u8,
}

static mut ARP_CACHE: [ArpEntry; ARP_CACHE_SIZE] = [ArpEntry {
    ip: 0,
    mac: MacAddr { b: [0; 6] },
    valid: 0,
}; ARP_CACHE_SIZE];

#[no_mangle]
pub extern "C" fn arp_init() {
    // SAFETY: static mutable cache during single-core kernel init.
    unsafe {
        for i in 0..ARP_CACHE_SIZE {
            ARP_CACHE[i].valid = 0;
        }
    }
}

#[no_mangle]
pub extern "C" fn arp_input(skb_ptr: *mut Skb) {
    if skb_ptr.is_null() {
        return;
    }
    // SAFETY: skb pointer valid for RX.
    unsafe {
        if skb::skb_len(skb_ptr) < core::mem::size_of::<ArpHeader>() as u16 {
            skb::skb_free(skb_ptr);
            return;
        }
        let ah = skb::skb_data(skb_ptr) as *mut ArpHeader;
        (*skb_ptr).arp = ah;

        let oper = u16::from_be((*ah).oper);
        let spa = u32::from_be((*ah).spa);
        let tpa = u32::from_be((*ah).tpa);

        let mut found = false;
        for i in 0..ARP_CACHE_SIZE {
            if ARP_CACHE[i].valid != 0 && ARP_CACHE[i].ip == spa {
                ARP_CACHE[i].mac = (*ah).sha;
                found = true;
                break;
            }
        }
        if !found {
            for i in 0..ARP_CACHE_SIZE {
                if ARP_CACHE[i].valid == 0 {
                    ARP_CACHE[i].ip = spa;
                    ARP_CACHE[i].mac = (*ah).sha;
                    ARP_CACHE[i].valid = 1;
                    break;
                }
            }
        }

        let my_ip = config::ip_host();
        if oper == 1 && tpa == my_ip {
            let reply = skb::skb_alloc();
            if !reply.is_null() {
                let rah =
                    skb::skb_push(reply, core::mem::size_of::<ArpHeader>() as u16) as *mut ArpHeader;
                (*rah).htype = (1u16).to_be();
                (*rah).ptype = (0x0800u16).to_be();
                (*rah).hlen = 6;
                (*rah).plen = 4;
                (*rah).oper = (2u16).to_be();
                (*rah).sha = runtime::my_mac;
                (*rah).spa = my_ip.to_be();
                (*rah).tha = (*ah).sha;
                (*rah).tpa = (*ah).spa;
                let _ = ethernet::ethernet_output(reply, (*ah).sha, ETH_TYPE_ARP);
                skb::skb_free(reply);
            }
        }
        skb::skb_free(skb_ptr);
    }
}

#[no_mangle]
pub extern "C" fn arp_request(target_ip_net: u32) {
    // SAFETY: stack-local packet build.
    unsafe {
        let skb_ptr = skb::skb_alloc();
        if skb_ptr.is_null() {
            return;
        }
        let ah = skb::skb_push(skb_ptr, core::mem::size_of::<ArpHeader>() as u16) as *mut ArpHeader;
        (*ah).htype = (1u16).to_be();
        (*ah).ptype = (0x0800u16).to_be();
        (*ah).hlen = 6;
        (*ah).plen = 4;
        (*ah).oper = (1u16).to_be();
        (*ah).sha = runtime::my_mac;
        (*ah).spa = config::ip_host().to_be();
        (*ah).tha = MacAddr { b: [0; 6] };
        (*ah).tpa = target_ip_net;
        let _ = ethernet::ethernet_output(skb_ptr, crate::types::MAC_BROADCAST, ETH_TYPE_ARP);
        skb::skb_free(skb_ptr);
    }
}

#[no_mangle]
pub extern "C" fn arp_lookup(ip_h: u32, out_mac: *mut MacAddr) -> i32 {
    if out_mac.is_null() {
        return 0;
    }
    // SAFETY: caller provides output pointer.
    unsafe {
        for i in 0..ARP_CACHE_SIZE {
            if ARP_CACHE[i].valid != 0 && ARP_CACHE[i].ip == ip_h {
                *out_mac = ARP_CACHE[i].mac;
                return 1;
            }
        }
    }
    0
}
