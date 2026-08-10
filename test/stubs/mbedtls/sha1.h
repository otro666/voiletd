#pragma once
#include <stddef.h>
typedef struct { int x; } mbedtls_sha1_context;
void mbedtls_sha1_init(mbedtls_sha1_context*);
void mbedtls_sha1_free(mbedtls_sha1_context*);
int mbedtls_sha1_starts(mbedtls_sha1_context*);
int mbedtls_sha1_update(mbedtls_sha1_context*, const unsigned char*, size_t);
int mbedtls_sha1_finish(mbedtls_sha1_context*, unsigned char*);
int mbedtls_sha1_ret(const unsigned char*, size_t, unsigned char*);
int mbedtls_sha1(const unsigned char*, size_t, unsigned char*);
