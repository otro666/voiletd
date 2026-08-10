#pragma once
#include <stddef.h>
typedef struct { int x; } mbedtls_entropy_context;
void mbedtls_entropy_init(mbedtls_entropy_context*);
int mbedtls_entropy_func(void*, unsigned char*, size_t);
