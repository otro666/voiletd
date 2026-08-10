#pragma once
#include <stdint.h>
#define ESP_OK 0
typedef int esp_err_t;
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr[4]; uint8_t zone; } esp_ip6_addr_t;
esp_netif_t* esp_netif_get_handle_from_ifkey(const char*);
esp_err_t esp_netif_get_ip6_global(esp_netif_t*, esp_ip6_addr_t*);
esp_err_t esp_netif_get_ip6_linklocal(esp_netif_t*, esp_ip6_addr_t*);
esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t*);
