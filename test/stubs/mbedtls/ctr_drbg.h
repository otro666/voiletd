#pragma once
#include <stddef.h>
typedef struct { int x; } mbedtls_ctr_drbg_context;
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context*);
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context*, int (*)(void*, unsigned char*, size_t), void*, const unsigned char*, size_t);
int mbedtls_ctr_drbg_random(void*, unsigned char*, size_t);
