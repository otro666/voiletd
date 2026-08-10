#pragma once
#include "ecp.h"
int mbedtls_ecdh_compute_shared(mbedtls_ecp_group*, mbedtls_mpi*, const mbedtls_ecp_point*,
                                const mbedtls_mpi*, int (*)(void*, unsigned char*, size_t), void*);
int mbedtls_ecp_check_pubkey(const mbedtls_ecp_group*, const mbedtls_ecp_point*);
