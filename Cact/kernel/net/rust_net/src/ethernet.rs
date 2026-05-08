use crate::arp;
use crate::ip;
use crate::runtime;
use crate::skb;
use crate::types::{EthHeader, MacAddr, Skb, ETH_TYPE_ARP, ETH_TYPE_IPV4};

fn mac_equal(a: MacAddr, b: MacAddr) -> bool {
    a.b == b.b
}

#[no_mangle]
pub extern "C" fn ethernet_input(skb_ptr: *mut Skb) {
    if skb_ptr.is_null() {
        return;
    }
    // SAFETY: packet ownership is transferred to stack.
    unsafe {
        if skb::skb_len(skb_ptr) < core::mem::size_of::<EthHeader>() as u16 {
            skb::skb_free(skb_ptr);
            return;
        }

        let eth = skb::skb_data(skb_ptr) as *mut EthHeader;
        (*skb_ptr).eth = eth;

        if !mac_equal((*eth).dst, runtime::my_mac)
            && !mac_equal((*eth).dst, crate::types::MAC_BROADCAST)
        {
            skb::skb_free(skb_ptr);
            return;
        }

        (*skb_ptr).data_offset += core::mem::size_of::<EthHeader>() as u16;
        (*skb_ptr).total_len -= core::mem::size_of::<EthHeader>() as u16;

        let et = u16::from_be((*eth).ethertype);
        if et == ETH_TYPE_ARP {
            arp::arp_input(skb_ptr);
            return;
        }
        if et == ETH_TYPE_IPV4 {
            ip::ip_input(skb_ptr);
            return;
        }
        skb::skb_free(skb_ptr);
    }
}

#[no_mangle]
pub extern "C" fn ethernet_output(skb_ptr: *mut Skb, dst: MacAddr, ethertype: u16) -> i32 {
    if skb_ptr.is_null() {
        return -1;
    }
    // SAFETY: skb pointer valid for TX path.
    unsafe {
        let hdr = skb::skb_push(skb_ptr, core::mem::size_of::<EthHeader>() as u16) as *mut EthHeader;
        if hdr.is_null() {
            return -1;
        }
        (*hdr).dst = dst;
        (*hdr).src = runtime::my_mac;
        (*hdr).ethertype = ethertype.to_be();

        if runtime::active_nic.is_null() {
            return -1;
        }
        if let Some(send) = (*runtime::active_nic).send {
            return send(skb_ptr);
        }
    }
    -1
}
