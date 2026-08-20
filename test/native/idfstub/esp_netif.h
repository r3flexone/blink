#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
extern esp_event_base_t IP_EVENT;
typedef enum { IP_EVENT_STA_GOT_IP = 0, IP_EVENT_STA_LOST_IP } ip_event_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff), \
                  (int)(((a)->addr >> 16) & 0xff), (int)(((a)->addr >> 24) & 0xff)
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *out);
