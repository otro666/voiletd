// Заглушка сокетов ПО ОБРАЗЦУ lwip, а не системных заголовков.
//
// Важно именно так: у lwip адрес IPv6 разложен как un.u32_addr, а у системной библиотеки
// иначе. Возьми мы системный вариант — проверка ругалась бы на правильный код и молчала
// бы о неправильном. Заглушка должна повторять ту библиотеку, что стоит на плате.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define AF_INET   2
#define AF_INET6  10
#define SOCK_DGRAM 2
#define SOL_SOCKET 0xfff
#define SO_REUSEADDR 0x0004
#define IPPROTO_IPV6 41
#define IPV6_V6ONLY 27
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 1
#define EWOULDBLOCK 11
#define EAGAIN 11

typedef uint32_t socklen_t;

struct in6_addr { union { uint32_t u32_addr[4]; uint8_t u8_addr[16]; } un; };
struct sockaddr { uint8_t sa_len, sa_family; char sa_data[14]; };
struct sockaddr_in6 {
    uint8_t sin6_len, sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

int socket(int, int, int);
int bind(int, const struct sockaddr*, socklen_t);
int setsockopt(int, int, int, const void*, socklen_t);
int fcntl(int, int, ...);
int close(int);
int sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
int recvfrom(int, void*, size_t, int, struct sockaddr*, socklen_t*);
uint16_t htons(uint16_t);
uint16_t ntohs(uint16_t);
uint32_t htonl(uint32_t);
uint32_t ntohl(uint32_t);
extern int errno;
