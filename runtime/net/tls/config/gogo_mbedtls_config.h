/* mbedTLS (TLS / X.509) configuration for the gogo "tls" port: a TLS 1.2
 * client only. See gogo_crypto_config.h for the crypto side and who uses
 * these files.
 *
 * TLS 1.3 is left out: mbedTLS 4.x does its TLS 1.3 key exchange through
 * PSA calls that cannot be sliced, while TLS 1.2 verifies ECDSA signatures
 * (ServerKeyExchange and the certificate chain) restartably. Every server
 * we target (Tailscale Funnel, Go, nginx, public CDNs) still speaks 1.2. */
#ifndef GOGO_MBEDTLS_CONFIG_H
#define GOGO_MBEDTLS_CONFIG_H

#define MBEDTLS_CONFIG_VERSION 0x04000000

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C

/* AEAD suites only (no CBC, so no encrypt-then-MAC). */
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256

/* Record buffers (spec: in 16 KB, servers do not negotiate a smaller
 * fragment; out 4 KB, our records are ours to size). */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif
