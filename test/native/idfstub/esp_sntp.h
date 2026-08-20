#pragma once
#include <stdbool.h>
typedef enum { ESP_SNTP_OPMODE_POLL = 0, ESP_SNTP_OPMODE_LISTENONLY } esp_sntp_operatingmode_t;
void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t m);
void esp_sntp_setservername(int idx, const char *server);
void esp_sntp_init(void);
void esp_sntp_stop(void);
bool esp_sntp_enabled(void);
