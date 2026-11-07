#ifndef NEXOS_NET_TLS_H
#define NEXOS_NET_TLS_H
/* net_tls.h -- glue between the kernel TCP client and the TLS 1.3 stack.
 *
 * net.cpp exposes a blocking byte stream (net_tcp_open/write/read); tls13.cpp
 * turns that into an authenticated, encrypted HTTP/1.1 conversation.  Nothing
 * here interprets application data.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Append a trust anchor (SHA-256 of the root certificate DER).
 * Returns 0 on success, -1 if the table is full.  With zero anchors every
 * handshake is refused -- there is no "trust anything" mode. */
int net_tls_add_anchor(const uint8_t fp_sha256[32]);
int net_tls_anchor_count(void);

/* POST an HTTPS request.  Returns bytes written to out (>0), or <0 on error
 * (see net_tls_last_error()). */
int net_https_post(const char* host, const char* path,
                   const char* extra_headers,
                   const char* body, char* out, int out_cap);

const char* net_tls_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
