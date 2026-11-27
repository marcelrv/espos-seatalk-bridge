/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * Signal K adapter (spec section 7), the espOS side. The pure parts (path
 * table, own-source rule, magnetic fallback, value parsing) are in
 * st_sk_map.c and unit-tested on a PC.
 *
 * Threading:
 *  - on_rx() runs on st_task (the bus task): it only calls the non-blocking
 *    espos_sk_publish_*().
 *  - on_sk() runs on espOS's Signal K stream task: it copies nothing it does
 *    not need, takes only st_core_set_value()'s short mutex and a short
 *    spinlock for the source filter, and never calls an espos_sk_* that
 *    waits. The fallback state (s_true, s_mag_ms, s_var_*) is touched only
 *    there, so it needs no lock.
 *  - the small "st_sk" task applies the config (the espos_config change
 *    callback only wakes it, debounced, because an import fires one callback
 *    per key) and publishes the diagnostics every 10 s while the Signal K
 *    stream is connected (never into the offline buffer). It is the only task
 *    that reads the config store.
 */
#include "st_sk.h"

#ifdef ESPOS_SEATALK_HAVE_SK

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "espos_config.h"
#include "espos_net.h"
#include "espos_sk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st_core.h"
#include "st_sk_map.h"

static const char *TAG = "st_sk";

#define CFG_NS "seatalk"
#define DIAG_PERIOD_MS 10000
#define HWM_LOG_MS 30000
#define CFG_DEBOUNCE_MS 150
#define TASK_STACK 4096
#define TASK_PRIO 3
#define FILTER_MAX 64
#define PATH_MAX_LEN 96 /* ESPOS_SK_PATH_MAX */

static char s_host[ESPOS_NET_HOSTNAME_MAX];
static bool s_started;
static TaskHandle_t s_task;

/* Written by one task each (published: st_task; the rest: the stream task);
 * read racily by st_sk_get_stats(), which is fine for 32-bit counters. */
static volatile st_sk_stats_t s_st;

/* Config the stream task reads. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_filter[ST_Q_COUNT][FILTER_MAX + 1]; /* guarded by s_mux */
static volatile bool s_mag_fallback = true;
static volatile uint32_t s_stale_ms = 5000;

/* Magnetic fallback state: index 0 = heading, 1 = course over ground.
 * Stream task only. */
typedef struct {
    double true_v;
    int64_t true_ms; /* ST_SK_NEVER = none */
    int64_t mag_ms;  /* last direct magnetic number */
} mag_state_t;
static mag_state_t s_mag[2] = {{0, ST_SK_NEVER, ST_SK_NEVER}, {0, ST_SK_NEVER, ST_SK_NEVER}};
static double s_var;
static int64_t s_var_ms = ST_SK_NEVER;

/* Substitution (SOG as speed, COG as heading). Modes written by the config
 * task, read by the stream task; the times are stream task only. */
static volatile st_sk_subst_t s_stw_subst = ST_SK_SUBST_OFF;
static volatile st_sk_subst_t s_hdg_subst = ST_SK_SUBST_OFF;
static int64_t s_stw_native_ms = ST_SK_NEVER; /* STW last set from its own path */
static int64_t s_hdg_native_ms = ST_SK_NEVER; /* heading last set (direct or derived) */

/* ------------------------------------------------------------ bus -> SK */

static void on_rx(st_link_event_t ev, const st_datagram_t *dg, st_dec_status_t ds, const st_decoded_t *dec,
                  bool forward, void *arg)
{
    (void)arg;
    if (ev != ST_LINK_DATAGRAM || !forward || ds != ST_DEC_OK || !dec) {
        return;
    }
    for (uint8_t i = 0; i < dec->n && i < ST_DEC_MAX_VALUES; i++) {
        const char *path = st_sk_path_of(dec->v[i].q);
        if (path && isfinite(dec->v[i].value) && espos_sk_publish_number(path, dec->v[i].value) == ESP_OK) {
            s_st.published++;
        }
    }
    if (dec->flags & ST_DF_SENSOR_DEFECTIVE) {
        const char *path = st_sk_defective_path(dg->b[0]);
        if (path && espos_sk_publish_json(path, "null") == ESP_OK) {
            s_st.published++;
        }
    }
}

/* ------------------------------------------------------------ SK -> bus */

static bool filter_allows(st_quantity_t q, const char *source)
{
    bool ok = true;
    portENTER_CRITICAL(&s_mux);
    if (s_filter[q][0] != '\0') {
        ok = source && strcmp(source, s_filter[q]) == 0;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static void set_q(st_quantity_t q, double v)
{
    if (st_core_set_value(q, v) == ESP_OK) {
        s_st.received++;
    }
}

/* A native STW or heading value: dropped in REPLACE mode, else set and
 * remembered so FALLBACK knows the native path is alive. NaN clears. */
static void set_native(st_quantity_t q, double v, int64_t now_ms)
{
    const bool stw = q == ST_Q_SPEED_THROUGH_WATER;
    if ((stw ? s_stw_subst : s_hdg_subst) == ST_SK_SUBST_REPLACE) {
        s_st.native_dropped++;
        return;
    }
    if (isfinite(v)) {
        *(stw ? &s_stw_native_ms : &s_hdg_native_ms) = now_ms;
    }
    set_q(q, v);
}

/* A SOG or COG value (NaN = cleared) also feeds speed / heading when the
 * substitution mode says so. */
static void substitute(st_quantity_t from, double v, int64_t now_ms)
{
    const bool sog = from == ST_Q_SPEED_OVER_GROUND;
    const st_sk_subst_t mode = sog ? s_stw_subst : s_hdg_subst;
    const int64_t native_ms = sog ? s_stw_native_ms : s_hdg_native_ms;
    if (st_sk_use_subst(mode, now_ms, native_ms, s_stale_ms) &&
        st_core_set_value(sog ? ST_Q_SPEED_THROUGH_WATER : ST_Q_HEADING_MAGNETIC, v) == ESP_OK) {
        s_st.substituted++;
    }
}

static void on_sk(const espos_sk_update_t *u, void *arg)
{
    const st_sk_sub_t *sub = (const st_sk_sub_t *)arg;
    if (!u || !u->value_json || !sub) { /* meta item */
        return;
    }
    if (st_sk_is_own_source(u->source, s_host)) {
        s_st.own_ignored++;
        return;
    }
    if (sub->q != ST_Q_NONE && !filter_allows(sub->q, u->source)) {
        s_st.filtered++;
        return;
    }
    double v = 0;
    const st_sk_val_t kind = st_sk_parse_value(u->value_json, &v);
    if (kind == ST_SK_VAL_BAD) {
        s_st.bad_value++;
        return;
    }
    const bool is_null = kind == ST_SK_VAL_NULL;
    const int64_t now_ms = esp_timer_get_time() / 1000;

    switch (sub->role) {
    case ST_SK_ROLE_DIRECT: {
        const double val = is_null ? NAN : v;
        if (!is_null) {
            if (sub->q == ST_Q_HEADING_MAGNETIC) {
                s_mag[0].mag_ms = now_ms;
            } else if (sub->q == ST_Q_COURSE_OVER_GROUND_MAGNETIC) {
                s_mag[1].mag_ms = now_ms;
            }
        }
        if (sub->q == ST_Q_SPEED_THROUGH_WATER || sub->q == ST_Q_HEADING_MAGNETIC) {
            set_native(sub->q, val, now_ms);
        } else {
            set_q(sub->q, val);
        }
        if (sub->q == ST_Q_SPEED_OVER_GROUND || sub->q == ST_Q_COURSE_OVER_GROUND_MAGNETIC) {
            substitute(sub->q, val, now_ms);
        }
        break;
    }
    case ST_SK_ROLE_VARIATION:
        s_var_ms = is_null ? ST_SK_NEVER : now_ms;
        s_var = v;
        break;
    case ST_SK_ROLE_TRUE_HDG:
    case ST_SK_ROLE_TRUE_COG: {
        mag_state_t *m = &s_mag[sub->role == ST_SK_ROLE_TRUE_HDG ? 0 : 1];
        if (is_null) {
            m->true_ms = ST_SK_NEVER;
            break;
        }
        m->true_v = v;
        m->true_ms = now_ms;
        double mag;
        if (st_sk_derive_mag(s_mag_fallback, now_ms, v, m->mag_ms, s_var, s_var_ms, s_stale_ms, &mag)) {
            s_st.mag_derived++;
            if (sub->role == ST_SK_ROLE_TRUE_HDG) {
                set_native(sub->q, mag, now_ms);
            } else {
                set_q(sub->q, mag);
                substitute(sub->q, mag, now_ms);
            }
        }
        break;
    }
    }
}

/* --------------------------------------------------------------- config */

static void apply_config(void)
{
    int32_t idle_us = 30000, jitter_us = 4000, stale_ms = 5000;
    bool mag_fb = true;
    (void)espos_config_get_i32(CFG_NS, "idle_us", &idle_us);
    (void)espos_config_get_i32(CFG_NS, "jitter_us", &jitter_us);
    (void)espos_config_get_i32(CFG_NS, "stale_ms", &stale_ms);
    (void)espos_config_get_bool(CFG_NS, "mag_fallback", &mag_fb);
    st_core_set_idle((uint32_t)idle_us, (uint32_t)jitter_us);
    s_stale_ms = (uint32_t)stale_ms;
    s_mag_fallback = mag_fb;

    static const struct {
        const char *key;
        volatile st_sk_subst_t *dst;
    } subst_keys[] = {{"stw_from_sog", &s_stw_subst}, {"hdg_from_cog", &s_hdg_subst}};
    for (size_t i = 0; i < sizeof subst_keys / sizeof subst_keys[0]; i++) {
        char val[16] = "";
        size_t len = 0;
        st_sk_subst_t m = ST_SK_SUBST_OFF;
        if (espos_config_get_str(CFG_NS, subst_keys[i].key, val, sizeof val, &len) != ESP_OK ||
            !st_sk_subst_from_name(val, &m)) {
            ESP_LOGW(TAG, "%s: unreadable value '%s', using off", subst_keys[i].key, val);
            m = ST_SK_SUBST_OFF;
        }
        *subst_keys[i].dst = m;
    }

    uint8_t n = 0;
    const st_dgram_info_t *tab = st_codec_table(&n);
    for (uint8_t i = 0; i < n; i++) {
        const st_dgram_info_t *t = &tab[i];
        char key[16];
        char val[16] = "";
        size_t len = 0;
        st_dgram_cfg_t c = {.mode = ST_MODE_AUTO, .period_ms = t->default_period_ms, .stale_ms = (uint32_t)stale_ms};

        snprintf(key, sizeof key, "m_%02x", t->cmd);
        if (espos_config_get_str(CFG_NS, key, val, sizeof val, &len) != ESP_OK || !st_mode_from_name(val, &c.mode)) {
            ESP_LOGW(TAG, "%s: unreadable mode '%s', using auto", key, val);
            c.mode = ST_MODE_AUTO;
        }
        if (t->can_encode) {
            int32_t p = (int32_t)t->default_period_ms;
            snprintf(key, sizeof key, "p_%02x", t->cmd);
            (void)espos_config_get_i32(CFG_NS, key, &p);
            c.period_ms = (uint32_t)p;
        } else {
            c.period_ms = 0;
        }
        const esp_err_t err = st_core_set_dgram_cfg(t->cmd, &c);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "datagram %02X (%s): st_core refused mode %s period %u ms: %s", t->cmd, t->name,
                     st_mode_name(c.mode), (unsigned)c.period_ms, esp_err_to_name(err));
        }
    }

    size_t nq = 0;
    const st_sk_qpath_t *qp = st_sk_qpaths(&nq);
    for (size_t i = 0; i < nq; i++) {
        char key[16];
        char val[FILTER_MAX + 1] = "";
        size_t len = 0;
        snprintf(key, sizeof key, "src_%s", qp[i].src);
        if (espos_config_get_str(CFG_NS, key, val, sizeof val, &len) != ESP_OK) {
            val[0] = '\0';
        }
        portENTER_CRITICAL(&s_mux);
        memcpy(s_filter[qp[i].q], val, sizeof val);
        portEXIT_CRITICAL(&s_mux);
    }
}

/* The descriptor is static JSON written by hand from st_codec_table(): make
 * a mismatch loud at start (a missing key would silently run on defaults). */
static void check_descriptor(void)
{
    const espos_cfg_ns_t *ns = espos_config_find_ns(CFG_NS);
    if (!ns) {
        ESP_LOGE(TAG, "config namespace '%s' is not registered", CFG_NS);
        return;
    }
    int missing = 0;
    char key[16];
    uint8_t n = 0;
    const st_dgram_info_t *tab = st_codec_table(&n);
    for (uint8_t i = 0; i < n; i++) {
        snprintf(key, sizeof key, "m_%02x", tab[i].cmd);
        if (!espos_config_find_key(ns, key)) {
            ESP_LOGE(TAG, "descriptor lacks key %s (%s)", key, tab[i].name);
            missing++;
        }
        if (tab[i].can_encode) {
            snprintf(key, sizeof key, "p_%02x", tab[i].cmd);
            if (!espos_config_find_key(ns, key)) {
                ESP_LOGE(TAG, "descriptor lacks key %s (%s)", key, tab[i].name);
                missing++;
            }
        }
    }
    static const char *const extra[] = {"idle_us", "jitter_us", "stale_ms", "mag_fallback", "stw_from_sog",
                                        "hdg_from_cog"};
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) {
        if (!espos_config_find_key(ns, extra[i])) {
            ESP_LOGE(TAG, "descriptor lacks key %s", extra[i]);
            missing++;
        }
    }
    size_t nq = 0;
    const st_sk_qpath_t *qp = st_sk_qpaths(&nq);
    for (size_t i = 0; i < nq; i++) {
        snprintf(key, sizeof key, "src_%s", qp[i].src);
        if (!espos_config_find_key(ns, key)) {
            ESP_LOGE(TAG, "descriptor lacks key %s", key);
            missing++;
        }
    }
    if (missing) {
        ESP_LOGE(TAG, "%d config keys missing: those settings run on their defaults", missing);
    }
}

static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)key;
    (void)arg;
    if (ns && strcmp(ns, CFG_NS) == 0 && s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* ---------------------------------------------------------- diagnostics */

typedef struct {
    const char *name;
    const char *units; /* "" = none */
    const char *desc;
} diag_t;

enum { D_TX_OK, D_TX_COLL, D_TX_DROP, D_TX_BUSY, D_RX_DG, D_RX_FLAG, D_RX_INC, D_LOAD, D_SK_PUB, D_SK_RX, D_SK_OWN, D_COUNT };

static const diag_t k_diag[D_COUNT] = {
    [D_TX_OK] = {"txOk", "", "SeaTalk datagrams sent without collision since start"},
    [D_TX_COLL] = {"txCollisions", "", "SeaTalk send attempts that collided"},
    [D_TX_DROP] = {"txDropped", "", "SeaTalk datagrams given up after retries"},
    [D_TX_BUSY] = {"txBusBusy", "", "Send attempts that found no quiet gap on the bus"},
    [D_RX_DG] = {"rxDatagrams", "", "Valid SeaTalk datagrams from other devices"},
    [D_RX_FLAG] = {"rxFlagged", "", "SeaTalk datagrams dropped for a doubtful byte"},
    [D_RX_INC] = {"rxIncomplete", "", "Partial SeaTalk datagrams"},
    [D_LOAD] = {"busLoad", "ratio", "SeaTalk bus load over the last 10 s (0 to 1)"},
    [D_SK_PUB] = {"skPublished", "", "Values sent to Signal K"},
    [D_SK_RX] = {"skReceived", "", "Signal K values handed to the SeaTalk side"},
    [D_SK_OWN] = {"skOwnIgnored", "", "Signal K values ignored because they came from this device"},
};

static char s_diag_base[48];

static void declare_diag_meta(void)
{
    int declared = 0;
    for (int i = 0; i < D_COUNT; i++) {
        char path[PATH_MAX_LEN];
        char meta[160];
        snprintf(path, sizeof path, "%s%s", s_diag_base, k_diag[i].name);
        if (k_diag[i].units[0]) {
            snprintf(meta, sizeof meta, "{\"units\":\"%s\",\"description\":\"%s\"}", k_diag[i].units, k_diag[i].desc);
        } else {
            snprintf(meta, sizeof meta, "{\"description\":\"%s\"}", k_diag[i].desc);
        }
        if (espos_sk_declare_meta(path, meta, DIAG_PERIOD_MS) == ESP_OK) {
            declared++;
        } else {
            ESP_LOGW(TAG, "meta for %s not declared (table full?)", path);
        }
    }
    ESP_LOGI(TAG, "%d meta entries declared (espOS health uses 9 more; limit %d)", declared, ESPOS_SK_MAX_META);
}

static void pub(int i, double v)
{
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof path, "%s%s", s_diag_base, k_diag[i].name);
    (void)espos_sk_publish_number(path, v);
}

static void publish_diag(void)
{
    static st_core_stats_t cs; /* only this task uses it */
    static uint64_t prev_bytes;
    static int64_t prev_us;
    st_core_get_stats(&cs);
    const uint64_t bytes = (uint64_t)cs.own_bytes + cs.foreign_bytes;
    const int64_t now_us = esp_timer_get_time();
    double load = 0;
    if (prev_us && bytes >= prev_bytes && now_us > prev_us) {
        load = (double)(bytes - prev_bytes) * (double)ST_CHAR_US / (double)(now_us - prev_us);
        if (load > 1.0) {
            load = 1.0;
        }
    }
    prev_bytes = bytes;
    prev_us = now_us;

    pub(D_TX_OK, cs.tx_ok);
    pub(D_TX_COLL, cs.tx_collisions);
    pub(D_TX_DROP, cs.tx_dropped);
    pub(D_TX_BUSY, cs.tx_bus_busy);
    pub(D_RX_DG, cs.link.datagrams);
    pub(D_RX_FLAG, cs.link.flagged);
    pub(D_RX_INC, cs.link.incomplete);
    pub(D_LOAD, load);
    pub(D_SK_PUB, s_st.published);
    pub(D_SK_RX, s_st.received);
    pub(D_SK_OWN, s_st.own_ignored);
}

static int32_t ticks_until(TickType_t now, TickType_t t)
{
    return (int32_t)(t - now);
}

static void sk_task(void *arg)
{
    (void)arg;
    const TickType_t diag_every = pdMS_TO_TICKS(DIAG_PERIOD_MS);
    const TickType_t t0 = xTaskGetTickCount();
    TickType_t next_diag = t0 + diag_every;
    const TickType_t hwm_at = t0 + pdMS_TO_TICKS(HWM_LOG_MS);
    bool hwm_done = false;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        int32_t wait = ticks_until(now, next_diag);
        if (!hwm_done && ticks_until(now, hwm_at) < wait) {
            wait = ticks_until(now, hwm_at);
        }
        if (wait < 0) {
            wait = 0;
        }
        if (ulTaskNotifyTake(pdTRUE, (TickType_t)wait) > 0) {
            vTaskDelay(pdMS_TO_TICKS(CFG_DEBOUNCE_MS)); /* let an import finish */
            (void)ulTaskNotifyTake(pdTRUE, 0);
            apply_config();
            ESP_LOGI(TAG, "config applied");
        }
        now = xTaskGetTickCount();
        if (ticks_until(now, next_diag) <= 0) {
            /* Only while the stream is up: offline, every diag delta would sit
             * in espos_sk's offline ring (~600 B per period) until it fills,
             * which on an ESP32 without PSRAM starves Wi-Fi/TLS and ends in an
             * espos_health restart. Only the latest diag values matter. */
            espos_sk_ws_status_t ws;
            if (espos_sk_ws_get_status(&ws) == ESP_OK && ws.connected) {
                publish_diag();
            }
            next_diag += diag_every;
            if (ticks_until(now, next_diag) < 0) {
                next_diag = now + diag_every;
            }
        }
        if (!hwm_done && ticks_until(now, hwm_at) <= 0) {
            hwm_done = true;
            ESP_LOGI(TAG, "task stack: %u of %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)TASK_STACK);
        }
    }
}

/* ---------------------------------------------------------------- start */

esp_err_t st_sk_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    espos_net_status_t net;
    s_host[0] = '\0';
    if (espos_net_get_status(&net) == ESP_OK) {
        snprintf(s_host, sizeof s_host, "%s", net.hostname);
    }
    if (!s_host[0]) {
        snprintf(s_host, sizeof s_host, "espos-%s", espos_net_short_id());
    }
    snprintf(s_diag_base, sizeof s_diag_base, "espos.%s.seatalk.", s_host);
    ESP_LOGI(TAG, "own Signal K source: %s (and %s.*)", s_host, s_host);

    check_descriptor();
    apply_config();

    esp_err_t err = st_core_add_rx(on_rx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st_core_add_rx failed: %s", esp_err_to_name(err));
        return err;
    }

    declare_diag_meta();

    size_t n = 0;
    const st_sk_sub_t *subs = st_sk_subs(&n);
    int ok = 0;
    for (size_t i = 0; i < n; i++) {
        if (espos_sk_subscribe(subs[i].path, subs[i].period_ms, on_sk, (void *)&subs[i]) > 0) {
            ok++;
        } else {
            ESP_LOGW(TAG, "subscribe %s failed", subs[i].path);
        }
    }
    ESP_LOGI(TAG, "%d of %u Signal K subscriptions", ok, (unsigned)n);

    if (xTaskCreate(sk_task, "st_sk", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    err = espos_config_subscribe(on_config_change, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config subscribe failed (%s): changes apply after a restart", esp_err_to_name(err));
    }
    s_started = true;
    return ESP_OK;
}

void st_sk_get_stats(st_sk_stats_t *out)
{
    if (!out) {
        return;
    }
    out->published = s_st.published;
    out->received = s_st.received;
    out->own_ignored = s_st.own_ignored;
    out->filtered = s_st.filtered;
    out->mag_derived = s_st.mag_derived;
    out->bad_value = s_st.bad_value;
    out->substituted = s_st.substituted;
    out->native_dropped = s_st.native_dropped;
}

const char *st_sk_own_label(void)
{
    return s_host;
}

#else /* !ESPOS_SEATALK_HAVE_SK */

#include <string.h>

esp_err_t st_sk_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void st_sk_get_stats(st_sk_stats_t *out)
{
    if (out) {
        memset(out, 0, sizeof *out);
    }
}

const char *st_sk_own_label(void)
{
    return "";
}

#endif /* ESPOS_SEATALK_HAVE_SK */
