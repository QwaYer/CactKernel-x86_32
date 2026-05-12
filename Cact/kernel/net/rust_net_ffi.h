#ifndef RUST_NET_FFI_H
#define RUST_NET_FFI_H

#include <stdint.h>

/*
 * FFI surface exported by Rust network module.
 * All IPv4 values here use host byte order unless noted otherwise.
 */
int rust_net_parse_ipv4(const char* input, uint32_t* out_host_ip);
int rust_net_ping_echo_host(uint32_t dst_ip_host, uint16_t id, uint16_t seq);
int rust_net_set_ipv4_config(uint32_t ip_h, uint32_t mask_h, uint32_t gw_h, uint32_t dns_h);
uint32_t rust_net_get_dns_host(void);
uint32_t rust_net_get_ip_host(void);
typedef struct {
    uint32_t ip_host;
    uint32_t netmask_host;
    uint32_t gateway_host;
    uint32_t dns_host;
    uint32_t server_host;
    uint32_t lease_s;
    uint32_t t1_s;
    uint32_t t2_s;
} rust_net_dhcp_lease_cfg_t;
int rust_net_dhcp_set_lease(const rust_net_dhcp_lease_cfg_t* cfg);
int rust_net_dns_resolve_a(const char* name, uint32_t* out_ip_host);

#endif /* RUST_NET_FFI_H */
