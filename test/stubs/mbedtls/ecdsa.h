#pragma once
#include "ecp.h"
#include "md.h"
typedef mbedtls_ecp_keypair mbedtls_ecdsa_context;
void mbedtls_ecdsa_init(mbedtls_ecdsa_context*);
void mbedtls_ecdsa_free(mbedtls_ecdsa_context*);
int  mbedtls_ecdsa_write_signature(mbedtls_ecdsa_context*, mbedtls_md_type_t,
                                   const unsigned char*, size_t, unsigned char*, size_t*,
                                   int (*)(void*, unsigned char*, size_t), void*);
int  mbedtls_ecdsa_read_signature(mbedtls_ecdsa_context*, const unsigned char*, size_t,
                                  const unsigned char*, size_t);
