#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
typedef struct esp_http_client *esp_http_client_handle_t;
typedef enum {
    HTTP_EVENT_ERROR = 0, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED, HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data; int data_len; void *user_data;
    char *header_key; char *header_value;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);
typedef struct {
    const char *url; const char *host; int port; const char *path;
    http_event_handle_cb event_handler;
    int timeout_ms; int buffer_size; int buffer_size_tx; void *user_data;
    bool disable_auto_redirect;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
