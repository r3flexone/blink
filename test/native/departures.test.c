// Kontrollierte HTTP-Antworten gegen die echte Parser-/Auswahllogik.
#include "host_compat.h"
#include <assert.h>
#include <time.h>
static time_t now_value;
static time_t test_time(time_t *out) { if (out) *out = now_value; return now_value; }
#define time test_time
#define esp_http_client_init test_http_init
#define esp_http_client_perform test_http_perform
#define esp_http_client_get_status_code test_http_status
#define esp_http_client_cleanup test_http_cleanup
#include "../../main/sbb.c"
static const char *payload;
esp_http_client_handle_t test_http_init(const esp_http_client_config_t *c) { (void)c; return (void *)1; }
esp_err_t test_http_perform(esp_http_client_handle_t c) {
    (void)c; strcpy(http_buf, payload); http_buf_len = (int)strlen(payload); return ESP_OK;
}
int test_http_status(esp_http_client_handle_t c) { (void)c; return 200; }
esp_err_t test_http_cleanup(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
static time_t stamp(const char *s) { time_t t; assert(departure_timestamp(s, &t)); return t; }
int main(void) {
    time_t t;
    assert(stamp("1970-01-01T00:00:00Z") == 0);
    assert(stamp("2026-09-08T12:00:00+0200") == 1788861600);
    assert(stamp("2026-09-08T12:00:00+02:00") == stamp("2026-09-08T10:00:00Z"));
    assert(stamp("2026-09-08T08:00:00-0200") == stamp("2026-09-08T10:00:00Z"));
    assert(stamp("2026-10-25T02:30:00+0100") - stamp("2026-10-25T02:30:00+0200") == 3600);
    assert(stamp("2024-03-01T00:00:00Z") - stamp("2024-02-29T00:00:00Z") == 86400);
    const char *invalid[] = {"12:00", "2026-02-29T12:00:00Z", "2026-09-08T24:00:00Z",
        "2026-09-08T12:00:00", "2026-09-08T12:00:00+02:99", "2026-09-08T12:00:00+0200x"};
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); i++) assert(!departure_timestamp(invalid[i], &t));
    wifi_ready = true; now_value = stamp("2026-09-08T12:00:30+0200");
    SbbDeparture out[DEP_COUNT];
    payload = "{\"stationboard\":[{\"stop\":{\"departure\":\"2026-09-08T11:59:00+0200\",\"delay\":5},\"to\":\"Delayed\"},{\"stop\":{\"departure\":\"2026-09-08T12:10:00+0200\"},\"to\":\"A\"},{\"stop\":{\"departure\":\"2026-09-08T12:20:00+0200\"},\"to\":\"B\"},{\"stop\":{\"departure\":\"2026-09-08T12:30:00+0200\"},\"to\":\"C\"},{\"stop\":{\"departure\":\"2026-09-08T12:40:00+0200\"},\"to\":\"D\"}]}";
    assert(sbb_get_departures("Test", out, NULL, 0));
    assert(strcmp(out[0].destination, "Delayed") == 0);
    assert(out[0].expectedDeparture == stamp("2026-09-08T12:04:00+0200"));
    payload = "{\"stationboard\":[{\"stop\":{\"departure\":\"2026-09-08T11:58:00+0200\"},\"to\":\"Past\"},{\"stop\":{\"departure\":\"2026-09-09T06:00:00+0200\"},\"to\":\"Tomorrow\"}]}";
    assert(sbb_get_departures("Test", out, NULL, 0));
    assert(strcmp(out[0].destination, "Tomorrow") == 0 && !out[1].valid);
    const char *filters[] = {"unmatched"};
    assert(!sbb_get_departures("Test", out, filters, 1));
    payload = "{\"stationboard\":[{\"stop\":{\"departure\":\"2026-09-08T12:05:00+0200\",\"delay\":20},\"to\":\"Later\"},{\"stop\":{\"departure\":\"2026-09-08T12:10:00+0200\"},\"to\":\"First\"}]}";
    assert(sbb_get_departures("Test", out, NULL, 0));
    assert(strcmp(out[0].destination, "First") == 0);
    now_value = stamp("2026-09-08T23:55:00+0200");
    payload = "{\"stationboard\":[{\"stop\":{\"departure\":\"2026-09-09T00:05:00+0200\"},\"to\":\"Midnight\"}]}";
    assert(sbb_get_departures("Test", out, NULL, 0));
    assert(out[0].expectedDeparture - now_value == 600);
    puts("Departure regressions passed");
    return 0;
}
