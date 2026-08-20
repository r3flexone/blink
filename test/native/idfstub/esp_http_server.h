#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct httpd_handle *httpd_handle_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3, HTTP_PUT = 4, HTTP_DELETE = 0, HTTP_OPTIONS = 6 } httpd_method_t;
typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    const char uri[512];
    size_t content_len;
    void *aux;
    void *user_ctx;
    void *sess_ctx;
} httpd_req_t;
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;
typedef struct {
    unsigned task_priority; size_t stack_size; int core_id;
    uint16_t server_port; uint16_t ctrl_port;
    uint16_t max_open_sockets; uint16_t max_uri_handlers;
    uint16_t max_resp_headers; uint16_t backlog_conn;
    bool lru_purge_enable; uint16_t recv_wait_timeout; uint16_t send_wait_timeout;
    bool uri_match_fn;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){ .task_priority = 5, .stack_size = 4096, \
    .server_port = 80, .max_uri_handlers = 8, .max_open_sockets = 7 })
typedef enum {
    HTTPD_400_BAD_REQUEST = 400, HTTPD_401_UNAUTHORIZED = 401,
    HTTPD_403_FORBIDDEN = 403, HTTPD_404_NOT_FOUND = 404, HTTPD_405_METHOD_NOT_ALLOWED = 405,
    HTTPD_408_REQ_TIMEOUT = 408, HTTPD_415_UNSUPPORTED_MEDIA_TYPE = 415,
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
} httpd_err_code_t;
#define HTTPD_SOCK_ERR_TIMEOUT (-408)
#define HTTPD_SOCK_ERR_INVALID (-405)
#define HTTPD_SOCK_ERR_FAIL    (-404)
esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t *c);
esp_err_t httpd_stop(httpd_handle_t h);
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *out, size_t len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t code, const char *msg);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
