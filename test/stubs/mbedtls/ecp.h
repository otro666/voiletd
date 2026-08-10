#pragma once
#include <stdint.h>
#include <stddef.h>
#define MBEDTLS_ECP_DP_SECP256R1 3
#define MBEDTLS_ECP_DP_SECP256K1 8
#define MBEDTLS_ECP_PF_UNCOMPRESSED 0
#define MBEDTLS_ECP_PF_COMPRESSED 1
typedef struct { int n; } mbedtls_mpi;
typedef struct { mbedtls_mpi X, Y, Z; } mbedtls_ecp_point;
typedef struct { mbedtls_mpi P, B, N; mbedtls_ecp_point G; } mbedtls_ecp_group;
typedef struct { mbedtls_ecp_group grp; mbedtls_mpi d; mbedtls_ecp_point Q; } mbedtls_ecp_keypair;
void mbedtls_ecp_group_init(mbedtls_ecp_group*);
void mbedtls_ecp_group_free(mbedtls_ecp_group*);
int  mbedtls_ecp_group_load(mbedtls_ecp_group*, int);
int  mbedtls_ecp_group_copy(mbedtls_ecp_group*, const mbedtls_ecp_group*);
void mbedtls_ecp_point_init(mbedtls_ecp_point*);
void mbedtls_ecp_point_free(mbedtls_ecp_point*);
int  mbedtls_ecp_point_read_binary(const mbedtls_ecp_group*, mbedtls_ecp_point*,
                                   const unsigned char*, size_t);
int  mbedtls_ecp_point_write_binary(const mbedtls_ecp_group*, const mbedtls_ecp_point*,
                                    int, size_t*, unsigned char*, size_t);
int  mbedtls_ecp_mul(mbedtls_ecp_group*, mbedtls_ecp_point*, const mbedtls_mpi*,
                     const mbedtls_ecp_point*, int (*)(void*, unsigned char*, size_t), void*);
int  mbedtls_ecp_gen_keypair(mbedtls_ecp_group*, mbedtls_mpi*, mbedtls_ecp_point*,
                             int (*)(void*, unsigned char*, size_t), void*);
void mbedtls_mpi_init(mbedtls_mpi*);
void mbedtls_mpi_free(mbedtls_mpi*);
int  mbedtls_mpi_read_binary(mbedtls_mpi*, const unsigned char*, size_t);
int  mbedtls_mpi_write_binary(const mbedtls_mpi*, unsigned char*, size_t);
int  mbedtls_mpi_copy(mbedtls_mpi*, const mbedtls_mpi*);
int  mbedtls_mpi_mul_mpi(mbedtls_mpi*, const mbedtls_mpi*, const mbedtls_mpi*);
int  mbedtls_mpi_mul_int(mbedtls_mpi*, const mbedtls_mpi*, uint32_t);
int  mbedtls_mpi_sub_mpi(mbedtls_mpi*, const mbedtls_mpi*, const mbedtls_mpi*);
int  mbedtls_mpi_add_mpi(mbedtls_mpi*, const mbedtls_mpi*, const mbedtls_mpi*);
int  mbedtls_mpi_add_int(mbedtls_mpi*, const mbedtls_mpi*, int32_t);
int  mbedtls_mpi_mod_mpi(mbedtls_mpi*, const mbedtls_mpi*, const mbedtls_mpi*);
int  mbedtls_mpi_exp_mod(mbedtls_mpi*, const mbedtls_mpi*, const mbedtls_mpi*,
                         const mbedtls_mpi*, mbedtls_mpi*);
int  mbedtls_mpi_shift_r(mbedtls_mpi*, size_t);
int  mbedtls_mpi_get_bit(const mbedtls_mpi*, size_t);
int  mbedtls_mpi_cmp_mpi(const mbedtls_mpi*, const mbedtls_mpi*);
int mbedtls_ecp_check_pubkey(const mbedtls_ecp_group*, const mbedtls_ecp_point*);
size_t mbedtls_mpi_size(const mbedtls_mpi*);
int mbedtls_mpi_lset(mbedtls_mpi*, int32_t);
int mbedtls_mpi_cmp_int(const mbedtls_mpi*, int32_t);
int mbedtls_mpi_inv_mod(mbedtls_mpi*, const mbedtls_mpi*, const mbedtls_mpi*);
int mbedtls_mpi_div_mpi(mbedtls_mpi*, mbedtls_mpi*, const mbedtls_mpi*, const mbedtls_mpi*);
