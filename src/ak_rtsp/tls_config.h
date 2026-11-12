/* mbedTLS configuration: HTTPS/RTSPS server and MQTT-over-TLS client, TLS 1.2, ECDHE-ECDSA with P-256, ChaCha20-Poly1305 (fast on ARM9 without AES
 * hardware) and AES-128-GCM, session tickets + cache so browsers resume
 * instead of redoing the expensive handshake, and X.509 writing for the
 * on-camera CA. Used via -DMBEDTLS_CONFIG_FILE="tls_config.h". */
#ifndef AK_TLS_CONFIG_H
#define AK_TLS_CONFIG_H

#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
/* mbedtls_ms_time() (TLS session ticket age) is provided by tls.c on top of
 * monotime.c: clock_gettime(CLOCK_MONOTONIC/BOOTTIME) is broken on this
 * camera with zig's musl, which made every resumed session look expired. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

/* crypto */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED   /* MQTT brokers (client side) */
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE 4
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 1
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C       /* X.509 key identifiers only */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_RSA_C                      /* brokers usually have RSA certificates */
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_ERROR_C

/* X.509 */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_X509_CSR_WRITE_C           /* MQTT client key made on the camera */

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_CLI_C                  /* MQTT over TLS */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_CACHE_C
#define MBEDTLS_SSL_TICKET_C
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384   /* browsers send full-size records */
#define MBEDTLS_SSL_OUT_CONTENT_LEN 16384

#endif
