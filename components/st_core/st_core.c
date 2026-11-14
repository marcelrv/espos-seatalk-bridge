/*
 * SeaTalk core task: RX path, polite TX with retries, safety (spec 2, 3.2, 5).
 */
#include "st_core.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "st_core";

#define SELFTEST_DATAGRAMS 16
#define SELFTEST_LEN 18
#define LOOP_MAX_SLEEP_MS 100

typedef struct {
    uint8_t len;
    uint8_t b[ST_MAX_DATAGRAM];
} once_item_t;

typedef enum { SEND_OK, SEND_COLLISION, SEND_NO_ECHO, SEND_BUSY } send_result_t;

static st_core_config_t s_cfg;
static SemaphoreHandle_t s_lock; /* guards everything below except the task-only state */
static TaskHandle_t s_task;
static QueueHandle_t s_once_hi, s_once_lo;
static SemaphoreHandle_t s_started;
static esp_err_t s_start_err;

/* guarded by s_lock */
static st_sched_t s_sched;
static st_core_stats_t s_st;
static struct {
    st_rx_cb_t cb;
    void *arg;
} s_rx[ST_CORE_MAX_RX_CB];
static volatile uint8_t s_n_rx;
static uint32_t s_idle_n[256], s_idle_min[256], s_idle_max[256];
static uint32_t s_idle_us, s_jitter_us;
static volatile bool s_paused;

/* task-only */
static st_link_t s_link;
static int64_t s_low_since_ms; /* 0 = line not low */
static int64_t s_no_echo_until_ms;

/* self-test handshake */
static SemaphoreHandle_t s_selftest_done;
static st_selftest_result_t *volatile s_selftest_req;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t rand_range(uint32_t lo, uint32_t hi)
{
    return hi > lo ? lo + esp_random() % (hi - lo + 1) : lo;
}

/* ------------------------------------------------------------------ RX */

static void process_rx(void)
{
    st_rx_char_t c;
    st_datagram_t dg;
    while (st_phy_rx_pop(&c)) {
        LOCK(); /* st_link's stats are read and reset from other tasks */
        const st_link_event_t ev = st_link_feed(&s_link, &c, &dg);
        if (c.flags & ST_F_ECHO) {
            s_st.own_bytes++;
        } else {
            s_st.foreign_bytes++;
        }
        UNLOCK();
        if (ev == ST_LINK_NONE) {
            continue;
        }

        st_decoded_t dec;
        st_dec_status_t ds = ST_DEC_UNKNOWN;
        bool forward = false;
        if (ev == ST_LINK_DATAGRAM) {
            ds = st_codec_decode(dg.b, dg.len, &dec);
        }
        LOCK();
        if (ev == ST_LINK_DATAGRAM) {
            forward = st_sched_on_foreign(&s_sched, dg.b[0], now_ms()) && ds == ST_DEC_OK;
            const uint8_t cmd = dg.b[0];
            if (s_idle_n[cmd] == 0 || dg.idle_before_us < s_idle_min[cmd]) {
                s_idle_min[cmd] = dg.idle_before_us;
            }
            if (dg.idle_before_us > s_idle_max[cmd]) {
                s_idle_max[cmd] = dg.idle_before_us;
            }
            s_idle_n[cmd]++;
        }
        const uint8_t n_cb = s_n_rx;
        UNLOCK();

        /* Outside the lock: a callback may call st_core_set_value(). The
         * table only grows and entries never change once set, so reading
         * the first n_cb entries without the lock is safe. */
        for (uint8_t i = 0; i < n_cb; i++) {
            s_rx[i].cb(ev, &dg, ds, ev == ST_LINK_DATAGRAM ? &dec : NULL, forward, s_rx[i].arg);
        }
    }
}

/* ------------------------------------------------------------------ safety */

static void check_bus_low(void)
{
    const bool low = st_phy_line_low();
    const int64_t t = now_ms();
    bool changed = false, held = false;
    if (low) {
        if (s_low_since_ms == 0) {
            s_low_since_ms = t;
        } else if (t - s_low_since_ms > ST_CORE_BUS_LOW_MS) {
            LOCK();
            changed = !s_st.bus_held_low;
            s_st.bus_held_low = true;
            UNLOCK();
            held = true;
        }
    } else {
        s_low_since_ms = 0;
        LOCK();
        changed = s_st.bus_held_low;
        s_st.bus_held_low = false;
        UNLOCK();
    }
    if (changed) {
        if (held) {
            ESP_LOGW(TAG, "bus held low for > %d s (no 12 V? short?): sending stopped",
                     ST_CORE_BUS_LOW_MS / 1000);
        } else {
            ESP_LOGW(TAG, "bus released: sending resumes");
        }
    }
}

static bool may_send(void)
{
    LOCK();
    const bool held = s_st.bus_held_low;
    bool no_echo = s_st.no_echo_pause;
    if (no_echo && now_ms() >= s_no_echo_until_ms) {
        s_st.no_echo_pause = no_echo = false;
        ESP_LOGI(TAG, "no-echo pause over: trying again");
    }
    UNLOCK();
    return !held && !no_echo;
}

/* ------------------------------------------------------------------ TX */

/* Polite idle wait, then one attempt. retries > 0 adds the collision back-off
 * on top of the idle wait (spec 5.2 rule 2). */
static send_result_t send_polite(const uint8_t *b, uint8_t len, uint8_t retries)
{
    LOCK();
    uint32_t quiet = s_idle_us + rand_range(0, s_jitter_us);
    UNLOCK();
    if (retries) {
        quiet += rand_range(ST_CORE_BACKOFF_MIN_US, ST_CORE_BACKOFF_MAX_US);
    }
    /* Wait in short slices and drain RX in between: foreign datagrams are
     * handled promptly, and the PHY's RX ring never has to hold more than
     * one slice of a busy bus (it is sized for that, see st_phy.c). */
    const int64_t t0 = esp_timer_get_time();
    const int64_t deadline = t0 + (int64_t)ST_CORE_QUIET_TIMEOUT_MS * 1000;
    bool quiet_ok = false;
    for (;;) {
        if (st_phy_wait_quiet(quiet, ST_CORE_WAIT_SLICE_MS)) {
            quiet_ok = true;
            break;
        }
        process_rx();
        if (esp_timer_get_time() >= deadline) {
            break;
        }
    }
    const uint32_t waited = (uint32_t)(esp_timer_get_time() - t0);

    LOCK();
    if (waited > s_st.idle_wait_max_us) {
        s_st.idle_wait_max_us = waited;
    }
    if (!quiet_ok) {
        s_st.tx_bus_busy++;
    }
    UNLOCK();
    if (!quiet_ok) {
        return SEND_BUSY;
    }

    const st_tx_result_t r = st_phy_send(b, len, NULL);
    switch (r) {
    case ST_TX_OK:
        return SEND_OK;
    case ST_TX_COLLISION:
        LOCK();
        s_st.tx_collisions++;
        UNLOCK();
        return SEND_COLLISION;
    default:
        LOCK();
        s_st.tx_no_echo++;
        if (!s_st.no_echo_pause) {
            s_st.no_echo_pause = true;
            s_no_echo_until_ms = now_ms() + ST_CORE_NO_ECHO_PAUSE_MS;
            ESP_LOGW(TAG, "no echo from the bus (%s): pausing TX for %d s", st_tx_result_name(r),
                     ST_CORE_NO_ECHO_PAUSE_MS / 1000);
        }
        UNLOCK();
        return SEND_NO_ECHO;
    }
}

static void count_result(send_result_t r, uint8_t retries)
{
    LOCK();
    if (r == SEND_OK) {
        s_st.tx_ok++;
        if (retries) {
            s_st.tx_retried_ok++;
        }
    } else if (r != SEND_COLLISION || retries >= ST_SCHED_MAX_RETRIES) {
        s_st.tx_dropped++;
    }
    UNLOCK();
}

/* One-off datagram with the full retry rules. */
static void send_once_item(const once_item_t *it)
{
    for (uint8_t retries = 0;; retries++) {
        process_rx();
        const send_result_t r = send_polite(it->b, it->len, retries);
        count_result(r, retries);
        if (r == SEND_COLLISION && retries < ST_SCHED_MAX_RETRIES) {
            continue;
        }
        LOCK();
        if (r == SEND_OK) {
            s_st.once_sent++;
        } else {
            s_st.once_dropped++;
        }
        UNLOCK();
        return;
    }
}

/* One periodic datagram, if one is due. Returns true when something was
 * attempted; *next_due gets the earliest upcoming due time. */
static bool send_periodic(int64_t *next_due)
{
    uint8_t buf[ST_MAX_DATAGRAM];
    uint8_t cmd = 0;
    LOCK();
    const uint8_t len = st_sched_next(&s_sched, now_ms(), buf, &cmd, next_due);
    const uint8_t retries = len ? st_sched_retries(&s_sched, cmd) : 0;
    UNLOCK();
    if (!len) {
        return false;
    }

    const send_result_t r = send_polite(buf, len, retries);
    count_result(r, retries);
    st_tx_result_t tr;
    switch (r) {
    case SEND_OK:
        tr = ST_TX_OK;
        break;
    case SEND_COLLISION:
        tr = ST_TX_COLLISION;
        break;
    case SEND_NO_ECHO:
        tr = ST_TX_NO_ECHO;
        break;
    default:
        tr = ST_TX_NOT_READY; /* bus busy: dropped, next period */
        break;
    }
    LOCK();
    st_sched_done(&s_sched, cmd, tr, now_ms());
    UNLOCK();
    return true;
}

/* ------------------------------------------------------------------ self-test */

static void run_selftest(st_selftest_result_t *res)
{
    st_phy_stats_t before, after;
    uint8_t buf[SELFTEST_LEN];
    memset(res, 0, sizeof(*res));
    process_rx();
    st_phy_get_stats(&before);
    for (int idx = 0; idx < SELFTEST_DATAGRAMS; idx++) {
        buf[0] = 0xF0; /* not an instrument datagram: displays ignore it */
        buf[1] = 0x0F;
        for (int i = 0; i < 16; i++) {
            buf[2 + i] = (uint8_t)(idx * 16 + i);
        }
        const send_result_t r = may_send() ? send_polite(buf, SELFTEST_LEN, 0) : SEND_BUSY;
        switch (r) {
        case SEND_OK:
            res->ok++;
            break;
        case SEND_COLLISION:
            res->collision++;
            break;
        case SEND_NO_ECHO:
            res->no_echo++;
            break;
        default:
            res->busy++;
            break;
        }
        process_rx();
    }
    st_phy_get_stats(&after);
    res->echo_bad = after.echo_bad - before.echo_bad;
    res->ambiguous = after.ambiguous - before.ambiguous;
    res->frame_err = after.frame_err - before.frame_err;
    res->pass = res->ok == SELFTEST_DATAGRAMS && res->echo_bad == 0 && res->ambiguous == 0 &&
                res->frame_err == 0;
}

/* ------------------------------------------------------------------ task */

static void st_task(void *arg)
{
    (void)arg;
    s_start_err = st_phy_init(&s_cfg.phy); /* ISR on this task's core */
    if (s_start_err == ESP_OK) {
        st_phy_set_rx_task(xTaskGetCurrentTaskHandle());
        s_start_err = st_phy_tx_enable(true);
    }
    xSemaphoreGive(s_started);
    if (s_start_err != ESP_OK) {
        ESP_LOGE(TAG, "PHY start failed: %s", esp_err_to_name(s_start_err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "running on core %d: idle %u us + 0..%u us jitter, planned %.1f datagrams/s",
             xPortGetCoreID(), (unsigned)s_idle_us, (unsigned)s_jitter_us, st_core_planned_rate());

    for (;;) {
        process_rx();
        check_bus_low();

        st_selftest_result_t *req = s_selftest_req;
        if (req) {
            run_selftest(req);
            s_selftest_req = NULL;
            xSemaphoreGive(s_selftest_done);
            continue;
        }

        int64_t next_due = INT64_MAX;
        if (may_send()) {
            once_item_t it;
            if (xQueueReceive(s_once_hi, &it, 0) == pdTRUE) {
                send_once_item(&it);
                continue;
            }
            /* NORMAL/LOW one-offs ($STALK) before periodic data. */
            if (xQueueReceive(s_once_lo, &it, 0) == pdTRUE) {
                send_once_item(&it);
                continue;
            }
            if (!s_paused && send_periodic(&next_due)) {
                continue;
            }
        }

        /* Nothing to send now: sleep until the next due slot, a received
         * character or LOOP_MAX_SLEEP_MS (bus-low check, queues). */
        int64_t sleep_ms = LOOP_MAX_SLEEP_MS;
        if (next_due != INT64_MAX) {
            const int64_t d = next_due - now_ms();
            if (d < sleep_ms) {
                sleep_ms = d < 1 ? 1 : d;
            }
        }
        TickType_t ticks = pdMS_TO_TICKS(sleep_ms);
        ulTaskNotifyTake(pdTRUE, ticks ? ticks : 1);
    }
}

/* ------------------------------------------------------------------ API */

esp_err_t st_core_start(const st_core_config_t *cfg)
{
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg = *cfg;
    s_idle_us = cfg->idle_us;
    s_jitter_us = cfg->jitter_us;
    s_lock = xSemaphoreCreateMutex();
    s_started = xSemaphoreCreateBinary();
    s_selftest_done = xSemaphoreCreateBinary();
    s_once_hi = xQueueCreate(ST_CORE_ONCE_QUEUE, sizeof(once_item_t));
    s_once_lo = xQueueCreate(ST_CORE_ONCE_QUEUE, sizeof(once_item_t));
    if (!s_lock || !s_started || !s_selftest_done || !s_once_hi || !s_once_lo) {
        return ESP_ERR_NO_MEM;
    }
    st_link_init(&s_link);
    st_sched_init(&s_sched, now_ms());
    memset(&s_st, 0, sizeof(s_st));
    s_st.since_us = esp_timer_get_time();

    const BaseType_t core = cfg->task_core < 0 ? tskNO_AFFINITY : cfg->task_core;
    if (xTaskCreatePinnedToCore(st_task, "st_task", cfg->task_stack, NULL, cfg->task_prio, &s_task,
                                core) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_started, portMAX_DELAY);
    if (s_start_err != ESP_OK) {
        s_task = NULL; /* it deleted itself: API calls must see "not running" */
    }
    return s_start_err;
}

esp_err_t st_core_add_rx(st_rx_cb_t cb, void *arg)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE; /* call after st_core_start() */
    }
    esp_err_t err = ESP_OK;
    LOCK();
    if (s_n_rx >= ST_CORE_MAX_RX_CB) {
        err = ESP_ERR_NO_MEM;
    } else {
        s_rx[s_n_rx].cb = cb;
        s_rx[s_n_rx].arg = arg;
        __asm__ __volatile__("memw"); /* entry visible before the count */
        s_n_rx = s_n_rx + 1;
    }
    UNLOCK();
    return err;
}

esp_err_t st_core_set_value(st_quantity_t q, double value)
{
    if ((int)q <= ST_Q_NONE || (int)q >= ST_Q_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    if (isnan(value)) {
        st_sched_clear_value(&s_sched, q);
    } else {
        st_sched_set_value(&s_sched, q, value, now_ms());
    }
    UNLOCK();
    if (s_task) {
        xTaskNotifyGive(s_task); /* a slot may have become sendable */
    }
    return ESP_OK;
}

esp_err_t st_core_set_dgram_cfg(uint8_t cmd, const st_dgram_cfg_t *cfg)
{
    LOCK();
    const bool ok = st_sched_set_cfg(&s_sched, cmd, cfg, now_ms());
    UNLOCK();
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t st_core_get_dgram_cfg(uint8_t cmd, st_dgram_cfg_t *cfg)
{
    LOCK();
    const bool ok = st_sched_get_cfg(&s_sched, cmd, cfg);
    UNLOCK();
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t st_core_get_slot(uint8_t cmd, st_slot_t *out)
{
    LOCK();
    const st_slot_t *sl = st_sched_slot(&s_sched, cmd);
    if (sl) {
        *out = *sl;
    }
    UNLOCK();
    return sl ? ESP_OK : ESP_ERR_NOT_FOUND;
}

double st_core_planned_rate(void)
{
    LOCK();
    const double r = st_sched_planned_rate(&s_sched);
    UNLOCK();
    return r;
}

esp_err_t st_core_send_once(const uint8_t *b, uint8_t len, st_prio_t prio)
{
    if (!b || len < 3 || len > ST_MAX_DATAGRAM || len != st_datagram_len(b[1])) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    once_item_t it = {.len = len};
    memcpy(it.b, b, len);
    if (xQueueSend(prio == ST_PRIO_HIGH ? s_once_hi : s_once_lo, &it, 0) != pdTRUE) {
        LOCK();
        s_st.once_queue_full++;
        UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void st_core_pause(bool pause)
{
    s_paused = pause;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool st_core_paused(void)
{
    return s_paused;
}

void st_core_set_idle(uint32_t idle_us, uint32_t jitter_us)
{
    LOCK();
    s_idle_us = idle_us;
    s_jitter_us = jitter_us;
    UNLOCK();
}

void st_core_get_stats(st_core_stats_t *out)
{
    LOCK();
    *out = s_st;
    out->paused = s_paused;
    out->stale_skips = 0;
    out->native_pauses = 0;
    for (uint8_t i = 0; i < s_sched.n_slots; i++) {
        out->stale_skips += s_sched.slot[i].st.stale_skips;
        out->native_pauses += s_sched.slot[i].st.native_pauses;
    }
    out->link = s_link.st;
    UNLOCK();
    st_phy_get_stats(&out->phy);
}

void st_core_reset_stats(void)
{
    LOCK();
    const bool held = s_st.bus_held_low, ne = s_st.no_echo_pause;
    memset(&s_st, 0, sizeof(s_st));
    s_st.bus_held_low = held;
    s_st.no_echo_pause = ne;
    s_st.since_us = esp_timer_get_time();
    st_sched_reset_stats(&s_sched);
    st_link_reset_stats(&s_link);
    memset(s_idle_n, 0, sizeof(s_idle_n));
    memset(s_idle_min, 0, sizeof(s_idle_min));
    memset(s_idle_max, 0, sizeof(s_idle_max));
    UNLOCK();
    st_phy_reset_stats();
}

void st_core_idle_before(uint8_t cmd, uint32_t *count, uint32_t *min_us, uint32_t *max_us)
{
    LOCK();
    if (count) {
        *count = s_idle_n[cmd];
    }
    if (min_us) {
        *min_us = s_idle_min[cmd];
    }
    if (max_us) {
        *max_us = s_idle_max[cmd];
    }
    UNLOCK();
}

esp_err_t st_core_selftest(st_selftest_result_t *out)
{
    if (!s_task || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s_task) {
        return ESP_ERR_INVALID_STATE; /* would deadlock */
    }
    LOCK();
    const bool busy = s_selftest_req != NULL;
    if (!busy) {
        s_selftest_req = out;
    }
    UNLOCK();
    if (busy) {
        return ESP_ERR_INVALID_STATE; /* another self-test is running */
    }
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_selftest_done, portMAX_DELAY);
    return ESP_OK;
}
