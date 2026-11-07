#ifndef NEXOS_TLS_X509_H
#define NEXOS_TLS_X509_H
/* tls_x509.h -- X.509 chain verification for the in-kernel TLS 1.3 client.
 *
 * SCOPE
 *   * DER parser for the certificate subset a server sends.
 *   * Signature verification: RSA PKCS#1 v1.5 + RSA-PSS (both SHA-256),
 *     ECDSA P-256 as an optional second path.
 *   * Chain walk child -> issuer, anchored at an explicitly trusted root.
 *   * hostname match against subjectAltName dNSName (CN fallback).
 *
 * TRUST MODEL
 *   There is no "accept everything" entry point.  A caller must supply the
 *   SHA-256 fingerprints of the trust anchors it accepts; a chain that does
 *   not terminate at one of them is rejected.  That keeps the kernel honest:
 *   adding a CA is an explicit, auditable act ("ai ca add <fingerprint>"),
 *   never a silent default.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Verify a TLS Certificate message payload:
 *   certs      -- concatenated DER certs, each prefixed by 3-byte length
 *   count      -- number of certs
 *   hostname   -- expected server name (dNSName match)
 *   anchors    -- array of 32-byte SHA-256 fingerprints of trusted roots
 *   n_anchors  -- number of anchors (0 => reject: nothing is trusted)
 * Returns 0 on success; <0 on failure with *reason set to a static string. */
int x509_verify_chain(const uint8_t* certs, int certs_len, int count,
                      const char* hostname,
                      const uint8_t* anchors, int n_anchors,
                      const char** reason);

/* Verify a CertificateVerify signature.
 *   scheme      -- TLS SignatureScheme (0x0804 rsa_pss_rsae_sha256,
 *                  0x0403 ecdsa_secp256r1_sha256, 0x0401 rsa_pkcs1_sha256)
 *   leaf        -- DER of the leaf certificate (without the 3-byte prefix)
 *   transcript  -- Transcript-Hash of the handshake
 * Returns 0 on success. */
int x509_verify_certificate_verify(const uint8_t* leaf, int leaf_len,
                                   uint16_t scheme,
                                   const uint8_t transcript[32],
                                   const uint8_t* sig, int sig_len);

#ifdef __cplusplus
}
#endif
#endif
