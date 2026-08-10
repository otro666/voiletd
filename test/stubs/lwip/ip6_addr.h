#pragma once
typedef struct { unsigned char addr[16]; } ip6_addr_t;
char* ip6addr_ntoa_r(const ip6_addr_t*, char*, int);
int ip6addr_aton(const char*, ip6_addr_t*);
