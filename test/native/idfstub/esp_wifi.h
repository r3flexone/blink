#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
extern esp_event_base_t WIFI_EVENT;
typedef enum {
    WIFI_EVENT_WIFI_READY = 0, WIFI_EVENT_SCAN_DONE,
    WIFI_EVENT_STA_START, WIFI_EVENT_STA_STOP,
    WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED,
    WIFI_EVENT_AP_START, WIFI_EVENT_AP_STOP,
    WIFI_EVENT_AP_STACONNECTED, WIFI_EVENT_AP_STADISCONNECTED,
} wifi_event_t;
typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP = 1 } wifi_interface_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK,
               WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK } wifi_auth_mode_t;
typedef struct {
    uint8_t ssid[32]; uint8_t password[64]; uint8_t ssid_len; uint8_t channel;
    wifi_auth_mode_t authmode; uint8_t ssid_hidden; uint8_t max_connection;
} wifi_ap_config_t;
typedef struct {
    uint8_t ssid[32]; uint8_t password[64]; uint8_t scan_method;
    bool bssid_set; uint8_t bssid[6]; uint8_t channel;
} wifi_sta_config_t;
typedef union { wifi_ap_config_t ap; wifi_sta_config_t sta; } wifi_config_t;
typedef struct { uint8_t bssid[6]; uint8_t ssid[33]; uint8_t primary; int8_t rssi; } wifi_ap_record_t;
typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){ .dummy = 0 })
esp_err_t esp_wifi_init(const wifi_init_config_t *c);
esp_err_t esp_wifi_set_mode(wifi_mode_t m);
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out);
