#include "host_compat.h"
#include <assert.h>
#define xTaskGetTickCount test_ticks
#define vTaskDelay test_delay
#define button_pressed test_button
#define button_wait_release test_release
#include "../../main/main.c"
static TickType_t ticks, trigger;
static int event;
TickType_t test_ticks(void) { return ticks; }
void test_delay(TickType_t delay) {
    ticks += delay;
    if (event == 1 && ticks >= trigger) g_cfg_dirty = true;
}
bool test_button(void) { return event == 2 && ticks >= trigger; }
void test_release(int timeout) { (void)timeout; }
int main(void) {
    blink_config_t a, b;
    nvs_config_defaults(&a); b = a;
    assert(!departure_query_changed(&a, &b));
    b.ledBrightness++; assert(!departure_query_changed(&a, &b));
    strcpy(b.station, "Basel SBB"); assert(departure_query_changed(&a, &b));
    b=a; b.destFilterCount=1; assert(departure_query_changed(&a, &b));
    a=b; strcpy(b.destFilters[0], "Olten"); assert(departure_query_changed(&a, &b));
    b=a; strcpy(b.destFilters[3], "inactive slot"); assert(!departure_query_changed(&a, &b));
    bool forced = false;
    ticks=0; event=0;
    assert(!active_wait(seconds_to_ticks(300), false, seconds_to_ticks(1), &forced));
    assert(ticks == seconds_to_ticks(1) && !forced);
    ticks=0; event=1; trigger=pdMS_TO_TICKS(200);
    assert(!active_wait(seconds_to_ticks(300), true, 0, &forced));
    assert(ticks <= pdMS_TO_TICKS(300) && g_cfg_dirty);
    g_cfg_dirty=false; ticks=0; event=2; trigger=pdMS_TO_TICKS(100);
    assert(!active_wait(seconds_to_ticks(300), true, 0, &forced));
    assert(forced && ticks <= pdMS_TO_TICKS(200));
    forced=false; event=0; ticks=0;
    assert(active_wait(seconds_to_ticks(300), true, 0, &forced));
    assert(ticks == seconds_to_ticks(300));
    ticks=UINT32_MAX-10; TickType_t start=ticks;
    assert(active_wait(30, true, 0, &forced));
    assert(ticks-start == 30);
    puts("Runtime interruption and cache-key regressions passed");
    return 0;
}
