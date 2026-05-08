use crate::arp;
use crate::config;
use crate::ethernet;
use crate::icmp;
use crate::skb;
use crate::tcp;
use crate::types::{IpHeader, MacAddr, Skb, ETH_TYPE_IPV4, IP_PROTO_ICMP, IP_PROTO_TCP, IP_PROTO_UDP};
use crate::udp;

static mut IP_ID_COUNTER: u16 = 1;

#[inline]
fn ip_hdr_len(h: &IpHeader) -> u16 {
    ((h.version_ihl & 0x0F) as u16) * 4
}

#[no_mangle]
pub extern "C" fn ip_input(skb_ptr: *mut Skb) {
    if skb_ptr.is_null() {
        return;
    }
    // SAFETY: packet parsing path.
    unsafe {
        if skb::skb_len(skb_ptr) < core::mem::size_of::<IpHeader>() as u16 {
            skb::skb_free(skb_ptr);
            return;
        }
        let iph = skb::skb_data(skb_ptr) as *mut IpHeader;
        (*skb_ptr).ip = iph;

        if (((*iph).version_ihl >> 4) & 0xF) != 4 {
            skb::skb_free(skb_ptr);
            return;
        }
        let ihl = ip_hdr_len(&*iph);
        if ihl < 20 || u16::from_be((*iph).total_len) < ihl {
            skb::skb_free(skb_ptr);
            return;
        }
        let saved = (*iph).checksum;
        (*iph).checksum = 0;
        if crate::checksum::inet_checksum(iph.cast::<u8>(), ihl) != saved {
            skb::skb_free(skb_ptr);
            return;
        }
        (*iph).checksum = saved;

        let dst = u32::from_be((*iph).dst_ip);
        let my_ip = config::ip_host();
        if dst != my_ip && dst != 0xFFFF_FFFF {
            skb::skb_free(skb_ptr);
            return;
        }

        (*skb_ptr).data_offset += ihl;
        (*skb_ptr).total_len -= ihl;

        match (*iph).protocol {
            IP_PROTO_ICMP => icmp::icmp_input(skb_ptr),
            IP_PROTO_UDP => udp::udp_input(skb_ptr),
            IP_PROTO_TCP => tcp::tcp_input(skb_ptr),
            _ => skb::skb_free(skb_ptr),
        }
    }
}

#[no_mangle]
pub extern "C" fn ip_output(skb_ptr: *mut Skb, dst_ip_net: u32, protocol: u8) -> i32 {
    if skb_ptr.is_null() {
        return -1;
    }
    // SAFETY: tx packet assembly path.
    unsafe {
        let payload_len = skb::skb_len(skb_ptr);
        let iph = skb::skb_push(skb_ptr, core::mem::size_of::<IpHeader>() as u16) as *mut IpHeader;
        if iph.is_null() {
            return -1;
        }
        (*iph).version_ihl = (4 << 4) | 5;
        (*iph).tos = 0;
        (*iph).total_len = (core::mem::size_of::<IpHeader>() as u16 + payload_len).to_be();
        (*iph).id = IP_ID_COUNTER.to_be();
        IP_ID_COUNTER = IP_ID_COUNTER.wrapping_add(1);
        (*iph).flags_frag = 0x4000u16.to_be();
        (*iph).ttl = 64;
        (*iph).protocol = protocol;
        (*iph).checksum = 0;
        let my_ip = config::ip_host();
        let my_mask = config::netmask_host();
        let my_gw = config::gateway_host();
        (*iph).src_ip = my_ip.to_be();
        (*iph).dst_ip = dst_ip_net;
        (*iph).checksum =
            crate::checksum::inet_checksum(iph.cast::<u8>(), core::mem::size_of::<IpHeader>() as u16);

        let dst_host = u32::from_be(dst_ip_net);
        let next_hop_net = if (dst_host & my_mask) == (my_ip & my_mask) {
            dst_ip_net
        } else {
            my_gw.to_be()
        };

        let mut next_mac = MacAddr { b: [0; 6] };
        if arp::arp_lookup(u32::from_be(next_hop_net), core::ptr::addr_of_mut!(next_mac)) == 0 {
            arp::arp_request(next_hop_net);
            skb::skb_free(skb_ptr);
            return -2;
        }
        let rc = ethernet::ethernet_output(skb_ptr, next_mac, ETH_TYPE_IPV4);
        skb::skb_free(skb_ptr);
        rc
    }
}

#[no_mangle]
pub extern "C" fn ip_pseudo_checksum(src: u32, dst: u32, proto: u8, len: u16) -> u16 {
    #[repr(C, packed)]
    struct Pseudo {
        src: u32,
        dst: u32,
        zero: u8,
        proto: u8,
        len: u16,
    }
    let p = Pseudo {
        src,
        dst,
        zero: 0,
        proto,
        len: len.to_be(),
    };
    crate::checksum::inet_checksum((&p as *const Pseudo).cast::<u8>(), core::mem::size_of::<Pseudo>() as u16)
}
