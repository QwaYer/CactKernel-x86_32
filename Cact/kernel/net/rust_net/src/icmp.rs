use crate::ip;
use crate::skb;
use crate::types::{IcmpHeader, Skb, ICMP_ECHO_REPLY, ICMP_ECHO_REQUEST, IP_PROTO_ICMP};

#[no_mangle]
pub extern "C" fn icmp_input(skb_ptr: *mut Skb) {
    if skb_ptr.is_null() {
        return;
    }
    // SAFETY: ICMP parse path.
    unsafe {
        if skb::skb_len(skb_ptr) < core::mem::size_of::<IcmpHeader>() as u16 {
            skb::skb_free(skb_ptr);
            return;
        }
        let icmp = skb::skb_data(skb_ptr) as *mut IcmpHeader;
        (*skb_ptr).icmp = icmp;

        let saved = (*icmp).checksum;
        (*icmp).checksum = 0;
        if crate::checksum::inet_checksum(icmp.cast::<u8>(), skb::skb_len(skb_ptr)) != saved {
            skb::skb_free(skb_ptr);
            return;
        }
        (*icmp).checksum = saved;

        if (*icmp).type_ == ICMP_ECHO_REPLY {
            skb::skb_free(skb_ptr);
            return;
        }
        if (*icmp).type_ != ICMP_ECHO_REQUEST {
            skb::skb_free(skb_ptr);
            return;
        }

        let payload_len = skb::skb_len(skb_ptr);
        let src_ip = (*(*skb_ptr).ip).src_ip;

        let reply = skb::skb_alloc();
        if reply.is_null() {
            skb::skb_free(skb_ptr);
            return;
        }
        let body = skb::skb_put(reply, payload_len);
        for i in 0..payload_len as usize {
            *body.add(i) = *(icmp.cast::<u8>().add(i));
        }
        let rh = body as *mut IcmpHeader;
        (*rh).type_ = ICMP_ECHO_REPLY;
        (*rh).code = 0;
        (*rh).checksum = 0;
        (*rh).checksum = crate::checksum::inet_checksum(rh.cast::<u8>(), payload_len);

        let _ = ip::ip_output(reply, src_ip, IP_PROTO_ICMP);
        skb::skb_free(reply);
        skb::skb_free(skb_ptr);
    }
}

#[no_mangle]
pub extern "C" fn icmp_send_echo_request(dst_ip: u32, id: u16, seq: u16) {
    const PAYLOAD: &[u8] = b"CactOS ping!";
    let total = (core::mem::size_of::<IcmpHeader>() + PAYLOAD.len()) as u16;
    // SAFETY: packet build path.
    unsafe {
        let skb_ptr = skb::skb_alloc();
        if skb_ptr.is_null() {
            return;
        }
        let icmp = skb::skb_put(skb_ptr, total) as *mut IcmpHeader;
        (*icmp).type_ = ICMP_ECHO_REQUEST;
        (*icmp).code = 0;
        (*icmp).checksum = 0;
        (*icmp).id = id.to_be();
        (*icmp).seq = seq.to_be();
        let data = icmp.add(1).cast::<u8>();
        for (i, b) in PAYLOAD.iter().enumerate() {
            *data.add(i) = *b;
        }
        (*icmp).checksum = crate::checksum::inet_checksum(icmp.cast::<u8>(), total);
        let _ = ip::ip_output(skb_ptr, dst_ip, IP_PROTO_ICMP);
    }
}
