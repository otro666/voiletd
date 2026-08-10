#pragma once
#include <stddef.h>
#define MBEDTLS_GCM_ENCRYPT 1
#define MBEDTLS_GCM_DECRYPT 0
#define MBEDTLS_CIPHER_ID_AES 2
typedef struct { int x; } mbedtls_gcm_context;
void mbedtls_gcm_init(mbedtls_gcm_context*);
void mbedtls_gcm_free(mbedtls_gcm_context*);
int mbedtls_gcm_setkey(mbedtls_gcm_context*, int, const unsigned char*, unsigned int);
int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context*, int, size_t, const unsigned char*,
                              size_t, const unsigned char*, size_t, const unsigned char*,
                              unsigned char*, size_t, unsigned char*);
int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context*, size_t, const unsigned char*, size_t,
                             const unsigned char*, size_t, const unsigned char*, size_t,
                             const unsigned char*, unsigned char*);
