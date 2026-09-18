#include "kvs_teardown_watch.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kvs_teardown";

/* Report a stage only once it has clearly overstayed. A healthy teardown runs
 * a couple of seconds end to end, so 3 s in a single stage means stuck, not
 * slow. */
#define STUCK_AFTER_US  (3 * 1000000LL)
#define WATCH_PERIOD_US (5 * 1000000LL)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_timer;
static char s_peer[48];
static const char *s_stage;
static int64_t s_begin_us;
static int64_t s_stage_us;
static bool s_active;
static TaskHandle_t s_task;

static const char *task_state_name(TaskHandle_t task)
{
    if (task == NULL) {
        return "?";
    }

    switch (eTaskGetState(task)) {
        case eRunning:   return "running";
        case eReady:     return "ready";
        case eBlocked:   return "blocked";
        case eSuspended: return "suspended";
        case eDeleted:   return "deleted";
        default:         return "?";
    }
}

static void watch_cb(void *arg)
{
    char peer[sizeof(s_peer)];
    const char *stage;
    int64_t stage_us, begin_us;
    TaskHandle_t task;
    bool active;

    (void) arg;

    portENTER_CRITICAL(&s_mux);
    active = s_active;
    stage = s_stage;
    stage_us = s_stage_us;
    begin_us = s_begin_us;
    task = s_task;
    memcpy(peer, s_peer, sizeof(peer));
    portEXIT_CRITICAL(&s_mux);

    if (!active) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if (now - stage_us < STUCK_AFTER_US) {
        return;
    }

    /* "blocked" here means waiting on a mutex, queue or socket rather than
     * spinning, which narrows the search to whoever holds the other side. */
    ESP_LOGW(TAG, "%s: stuck in '%s' for %lld ms (%lld ms into teardown, task %s)",
             peer, stage ? stage : "?", (now - stage_us) / 1000,
             (now - begin_us) / 1000, task_state_name(task));
}

void kvs_teardown_watch_begin(const char *peer)
{
    if (s_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = watch_cb,
            .name = "kvs_teardown",
        };

        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            s_timer = NULL;
        }
    }

    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    strlcpy(s_peer, peer != NULL ? peer : "?", sizeof(s_peer));
    s_stage = "begin";
    s_begin_us = now;
    s_stage_us = now;
    s_active = true;
    s_task = xTaskGetCurrentTaskHandle();
    portEXIT_CRITICAL(&s_mux);

    /* Already running is the normal case after the first teardown. */
    if (s_timer != NULL) {
        esp_timer_start_periodic(s_timer, WATCH_PERIOD_US);
    }
}

void kvs_teardown_watch_stage(const char *stage)
{
    int64_t now = esp_timer_get_time();
    char peer[sizeof(s_peer)];
    const char *prev;
    int64_t prev_us;
    bool active;

    portENTER_CRITICAL(&s_mux);
    active = s_active;
    prev = s_stage;
    prev_us = s_stage_us;
    memcpy(peer, s_peer, sizeof(peer));
    if (active) {
        s_stage = stage;
        s_stage_us = now;
    }
    portEXIT_CRITICAL(&s_mux);

    if (!active) {
        return;
    }

    /* The whole trail is printed, not just the slow steps. The stage that
     * hangs never prints at all -- it is the one the timer names -- so the
     * trail is what says which steps completed before it. */
    ESP_LOGW(TAG, "%s: %s (previous stage '%s' took %lld ms)",
             peer, stage != NULL ? stage : "?", prev != NULL ? prev : "?",
             (now - prev_us) / 1000);
}

void kvs_teardown_watch_end(void)
{
    int64_t now = esp_timer_get_time();
    char peer[sizeof(s_peer)];
    const char *prev;
    int64_t prev_us, begin_us;
    bool active;

    portENTER_CRITICAL(&s_mux);
    active = s_active;
    prev = s_stage;
    prev_us = s_stage_us;
    begin_us = s_begin_us;
    memcpy(peer, s_peer, sizeof(peer));
    s_active = false;
    s_stage = NULL;
    s_task = NULL;
    portEXIT_CRITICAL(&s_mux);

    if (!active) {
        return;
    }

    if (s_timer != NULL) {
        esp_timer_stop(s_timer);
    }

    ESP_LOGW(TAG, "%s: teardown complete in %lld ms (last stage '%s' took %lld ms)",
             peer, (now - begin_us) / 1000, prev != NULL ? prev : "?",
             (now - prev_us) / 1000);
}
