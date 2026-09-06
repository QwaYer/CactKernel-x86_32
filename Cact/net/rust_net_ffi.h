#ifndef RUST_NET_FFI_H
#define RUST_NET_FFI_H

#include <stdint.h>

/*
 * FFI surface exported by the Rust network module (cact_net).
 * The whole L3+ stack (Ethernet demux, ARP, IPv4, ICMP, TCP, UDP, DNS,
 * TLS 1.3 via rustls, HTTP/HTTPS) lives in Rust; the C kernel side calls into
 * these entry points.  The kernel does not run a DHCP client: addressing is
 * configured by userspace through the `rust_net_set_ipv4_config` / netcfg path.
 *
 * All IPv4 values here use host byte order unless noted otherwise.
 */
int rust_net_parse_ipv4(const char* input, uint32_t* out_host_ip);
int rust_net_ping_echo_host(uint32_t dst_ip_host, uint16_t id, uint16_t seq);
int rust_net_set_ipv4_config(uint32_t ip_h, uint32_t mask_h, uint32_t gw_h, uint32_t dns_h);
uint32_t rust_net_get_dns_host(void);
uint32_t rust_net_get_ip_host(void);
/* Current IPv4 link configuration snapshot (host byte order); NULL pointers skipped. */
int rust_net_get_ipv4_config(uint32_t* ip_h, uint32_t* mask_h, uint32_t* gw_h, uint32_t* dns_h);
/* Link state: 1 while a NIC driver is registered, 0 otherwise. */
int rust_net_link_is_up(void);
/* Copy the registered NIC MAC address into out[6]; returns 0, or -1 when no NIC/NULL. */
int rust_net_get_mac(uint8_t out[6]);
int rust_net_dns_resolve_a(const char* name, uint32_t* out_ip_host);

/*
 * ── TLS (rustls, in-kernel) ──────────────────────────────────────────────
 * Operates over an already-connected TCP socket (socket index from tcp_socket()).
 */
/* Connect TLS over an open connected TCP socket. Legacy entry: verified path. */
int cact_tls_connect(int sock, const char* server_name);
/* skip_verify != 0 disables certificate chain verification (the in-kernel
   cact_crypto provider does not implement cert signature verification yet). */
int cact_tls_connect_ex(int sock, const char* server_name, int skip_verify);
int cact_tls_send(int conn, const void* data, uint16_t len);
int cact_tls_recv(int conn, void* buf, uint16_t max_len);  /* 0 = no data yet, -1 = error/closed */
void cact_tls_close(int conn);

/*
 * ── HTTP / HTTPS client ───────────────────────────────────────────────────
 * Synchronous in-kernel fetcher: DNS -> TCP connect -> (TLS handshake) ->
 * HTTP/1.1 request -> response buffered into out_buf.  Response headers and
 * body are both written into out_buf; body_off/body_len point at the body.
 */
typedef enum {
    CACT_HTTP_GET    = 1,
    CACT_HTTP_POST   = 2,
    CACT_HTTP_PUT    = 3,
    CACT_HTTP_DELETE = 4,
    CACT_HTTP_HEAD   = 5,
} cact_http_method_t;

/* cact_http_request flags. By default HTTPS skips certificate chain
   verification (the in-kernel cact_crypto provider has no certificate
   signature verification yet). Set CACT_HTTP_FLAG_VERIFY_TLS to require a
   verified chain (fails against servers whose certs cannot be verified). */
#define CACT_HTTP_FLAG_VERIFY_TLS 0x1

typedef struct {
    uint16_t status;     /* HTTP status code (200, 404, ...); 0 = transport failure */
    uint32_t body_off;   /* byte offset of the body inside out_buf (after headers)   */
    uint32_t body_len;   /* bytes of the decoded body written into out_buf           */
    uint32_t total;      /* total bytes written into out_buf (headers + body)        */
    uint8_t  truncated;  /* 1 = out_buf too small, response was cut                  */
    uint8_t  chunked;    /* 1 = body was Transfer-Encoding: chunked (decoded)        */
    uint8_t  tls;        /* 1 = the exchange used TLS                                */
    uint8_t  timed_out;  /* 1 = request hit its deadline                              */
} cact_http_resp_t;

/* Generic request. url: "http://host[:port]/path" or "https://host[:port]/path".
   headers: optional extra request headers, CRLF-separated (may be NULL).
   body/body_len: optional request body (POST/PUT).
   flags: CACT_HTTP_FLAG_VERIFY_TLS or 0.
   Returns 0 + filled *out on success; -1 on transport/DNS/TLS failure;
   -2 when out_len cannot even hold the response headers. */
int cact_http_request(cact_http_resp_t* out, const char* url, int method,
                      const char* headers, const void* body, uint32_t body_len,
                      uint32_t flags, void* out_buf, uint32_t out_len);

/* Convenience wrappers (flags as in cact_http_request). */
int cact_http_get(cact_http_resp_t* out, const char* url, const char* headers,
                  uint32_t flags, void* out_buf, uint32_t out_len);
int cact_http_post(cact_http_resp_t* out, const char* url, const char* headers,
                   const void* body, uint32_t body_len, uint32_t flags,
                   void* out_buf, uint32_t out_len);

#endif /* RUST_NET_FFI_H */
