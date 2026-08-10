#pragma once
#include <stddef.h>
typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA1, MBEDTLS_MD_SHA256 } mbedtls_md_type_t;
typedef struct mbedtls_md_info_t mbedtls_md_info_t;
typedef struct { int x; } mbedtls_md_context_t;
const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t);
int mbedtls_md(const mbedtls_md_info_t*, const unsigned char*, size_t, unsigned char*);
void mbedtls_md_init(mbedtls_md_context_t*);
void mbedtls_md_free(mbedtls_md_context_t*);
int mbedtls_md_setup(mbedtls_md_context_t*, const mbedtls_md_info_t*, int);
int mbedtls_md_hmac_starts(mbedtls_md_context_t*, const unsigned char*, size_t);
int mbedtls_md_hmac_update(mbedtls_md_context_t*, const unsigned char*, size_t);
int mbedtls_md_hmac_finish(mbedtls_md_context_t*, unsigned char*);
int mbedtls_md_hmac_reset(mbedtls_md_context_t*);
int mbedtls_md_hmac(const mbedtls_md_info_t*, const unsigned char*, size_t,
                    const unsigned char*, size_t, unsigned char*);
size_t mbedtls_md_get_size(const mbedtls_md_info_t*);
