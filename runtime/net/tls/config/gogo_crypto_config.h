/* TF-PSA-Crypto configuration for the gogo "tls" port (runtime/net/tls/tls.c): only what
 * a TLS 1.2 client with ECDHE-ECDSA / ECDHE-RSA, AES-GCM and
 * ChaCha20-Poly1305 and X.509 chain verification needs, with restartable
 * (interruptible) ECC so signature checks are sliced. Used by the host build
 * (Makefile, ESP-IDF's mbedTLS 4.x source tree) and meant for any platform
 * that builds mbedTLS itself (RP2040); ESP-IDF configures its own copy
 * through sdkconfig and must match the choices marked "needed". */
#ifndef GOGO_CRYPTO_CONFIG_H
#define GOGO_CRYPTO_CONFIG_H

#define TF_PSA_CRYPTO_CONFIG_VERSION 0x01000000

/* Algorithms (needed). */
#define PSA_WANT_ALG_ECDH                       1
#define PSA_WANT_ALG_ECDSA                      1
#define PSA_WANT_ALG_GCM                        1
#define PSA_WANT_ALG_CHACHA20_POLY1305          1
#define PSA_WANT_ALG_HMAC                       1
#define PSA_WANT_ALG_SHA_256                    1
#define PSA_WANT_ALG_SHA_384                    1
#define PSA_WANT_ALG_SHA_512                    1
#define PSA_WANT_ALG_TLS12_PRF                  1
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN          1
#define PSA_WANT_ALG_RSA_PSS                    1
/* Curves: P-256 and P-384 (ECDHE and certificate keys) and X25519 (ECDHE).
 * No ECDHE curve is sliced in mbedTLS 4.1 (the key exchange is plain PSA
 * calls); X25519 is the cheaper one on the ESP32-S3 (204 ms against about
 * 300 ms for P-256), so tls.c prefers it, here as there (the
 * ESP build has it through CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED). */
#define PSA_WANT_ECC_SECP_R1_256                1
#define PSA_WANT_ECC_SECP_R1_384                1
#define PSA_WANT_ECC_MONTGOMERY_255             1
/* Key types. */
#define PSA_WANT_KEY_TYPE_AES                   1
#define PSA_WANT_KEY_TYPE_CHACHA20              1
#define PSA_WANT_KEY_TYPE_DERIVE                1
#define PSA_WANT_KEY_TYPE_HMAC                  1
#define PSA_WANT_KEY_TYPE_RAW_DATA              1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY        1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC    1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT   1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE 1
#define PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY        1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC    1   /* mbedTLS requires it for ECDHE-RSA */

/* Core. Randomness comes from platform_random() through the external RNG
 * hook (runtime/net/tls/tls.c), so no entropy module and no DRBG are linked. */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC
/* PSA would otherwise copy every input and output buffer (a 16 KB record
 * twice) to guard against memory shared with an untrusted caller; mbedTLS is
 * its only caller here. */
#define MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS

/* Platform: memory through platform_tls_alloc / platform_tls_free, wall
 * clock through vm_unix_time_ms (certificate validity). */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT   /* mbedtls_ms_time: platform_now_ms (tls.c) */

/* Parsing (X.509 and PEM bundles). */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_MD_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* Sliced ECC (needed): signature checks give up after
 * psa_interruptible_set_max_ops() basic operations and resume later. */
#define MBEDTLS_ECP_RESTARTABLE
#define MBEDTLS_ECP_NIST_OPTIM

/* Size over speed where it is cheap. */
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_MPI_MAX_SIZE 512    /* RSA keys up to 4096 bits */

#endif
