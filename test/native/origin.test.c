#include "host_compat.h"
#include <assert.h>
#define httpd_req_get_hdr_value_len test_header_len
#define httpd_req_get_hdr_value_str test_header_get
#include "../../main/http_server.c"
static const char *origin;
static bool read_error;
size_t test_header_len(httpd_req_t *r, const char *key) {
    (void)r; return strcmp(key, "Origin") == 0 && origin ? strlen(origin) : 0;
}
esp_err_t test_header_get(httpd_req_t *r, const char *key, char *out, size_t len) {
    (void)r;
    const char *s = strcmp(key, "Origin") == 0 ? origin : "sbb-monitor.local";
    if (read_error || !s || strlen(s) >= len) return ESP_FAIL;
    strcpy(out, s); return ESP_OK;
}
int main(void) {
    httpd_req_t req = {0};
    origin = NULL; assert(origin_is_self(&req));
    origin = "http://sbb-monitor.local"; assert(origin_is_self(&req));
    origin = "http://example.invalid"; assert(!origin_is_self(&req));
    origin = "null"; assert(!origin_is_self(&req));
    char long_origin[200]; memset(long_origin, 'a', sizeof(long_origin)-1); long_origin[199] = 0;
    origin = long_origin;
    assert(!origin_is_self(&req));
    assert(require_write_access(&req, false) == ESP_FAIL);
    assert(require_write_access(&req, true) == ESP_FAIL);
    origin = "http://sbb-monitor.local"; read_error = true; assert(!origin_is_self(&req));
    puts("Origin regressions passed");
    return 0;
}
