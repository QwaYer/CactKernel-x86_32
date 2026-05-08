pub fn inet_checksum(mut data: *const u8, mut len: u16) -> u16 {
    let mut sum: u32 = 0;
    // SAFETY: caller guarantees valid memory region.
    unsafe {
        while len > 1 {
            let v = u16::from_le_bytes([*data, *data.add(1)]);
            sum = sum.wrapping_add(v as u32);
            data = data.add(2);
            len -= 2;
        }
        if len != 0 {
            sum = sum.wrapping_add(*data as u32);
        }
    }
    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}
