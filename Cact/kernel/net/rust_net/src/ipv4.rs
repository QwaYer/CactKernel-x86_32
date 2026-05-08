/* ────────────────────────────────────────────────────────────────────────── */
/*  IPv4 helpers (host byte order)                                           */
/* ────────────────────────────────────────────────────────────────────────── */

pub fn parse_ipv4_host(s: &[u8]) -> Option<u32> {
    let mut acc: u32 = 0;
    let mut dots = 0usize;
    let mut part: u32 = 0;
    let mut has_digit = false;

    for &b in s {
        if b.is_ascii_digit() {
            part = part.saturating_mul(10).saturating_add((b - b'0') as u32);
            if part > 255 {
                return None;
            }
            has_digit = true;
            continue;
        }

        if b == b'.' {
            if !has_digit || dots >= 3 {
                return None;
            }
            acc = (acc << 8) | part;
            dots += 1;
            part = 0;
            has_digit = false;
            continue;
        }

        return None;
    }

    if !has_digit || dots != 3 {
        return None;
    }

    Some((acc << 8) | part)
}

#[inline]
pub fn htonl(v: u32) -> u32 {
    v.swap_bytes()
}
