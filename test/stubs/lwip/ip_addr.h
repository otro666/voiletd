#pragma once
#include <stdint.h>
#define IPADDR_TYPE_V4 0
typedef struct { uint32_t addr; uint8_t type; } ip_addr_t;
int ipaddr_aton(const char*, ip_addr_t*);
uint32_t ip_addr_get_ip4_u32(const ip_addr_t*);
