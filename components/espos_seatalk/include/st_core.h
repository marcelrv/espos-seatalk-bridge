/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * SeaTalk core (spec section 2): the one task that runs the bus.
 *
 * st_task owns st_phy, st_link and st_sched:
 *   RX  : st_phy chars -> st_link datagrams -> st_codec values -> rx callback
 *         (with a "forward to Signal K" verdict from the per-type mode)
 *   TX  : one-off queue (HIGH first) and st_sched's periodic slots ->
 *         polite idle wait (idle_us + random 0..jitter_us of quiet bus) ->
 *         st_phy_send -> retry after a collision with a random 2..15 ms
 *         back-off on top of the idle wait, at most 5 retries
 *   SAFETY: no-echo pauses sending for ST_CORE_NO_ECHO_PAUSE_MS; a bus held
 *         low for ST_CORE_BUS_LOW_MS stops sending until it is released.
 *
 * All functions are thread-safe unless noted. Adapters (Signal K, NMEA 0183,
 * bench console) use only this API.
 */
#pragma once

#include "esp_err.h"
#include "st_codec.h"
#include "st_link.h"
#include "st_phy.h"
#include "st_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_CORE_BUS_LOW_MS 10000      /* spec 3.2 rule 5 */
#define ST_CORE_NO_ECHO_PAUSE_MS 5000 /* rule 3: report, pause, try again later */
#define ST_CORE_BACKOFF_MIN_US 2000   /* rule 2 */
#define ST_CORE_BACKOFF_MAX_US 15000
#define ST_CORE_QUIET_TIMEOUT_MS 2000 /* no quiet gap this long: bus-busy */
#define ST_CORE_WAIT_SLICE_MS 50      /* RX is drained at least this often while waiting */
#define ST_CORE_ONCE_QUEUE 8

typedef struct {
    st_phy_config_t phy;
    uint32_t idle_us;   /* quiet bus before sending (spec 5.2 rule 1): 30000 */
    uint32_t jitter_us; /* + random 0..jitter_us: 4000 */
    int task_core;      /* core for st_task (and so the UART ISR); -1 = no affinity */
    int task_prio;
    uint32_t task_stack;
} st_core_config_t;

#define ST_CORE_CONFIG_DEFAULT()                                                         \
    {                                                                                    \
        .phy = ST_PHY_CONFIG_DEFAULT(), .idle_us = 30000, .jitter_us = 4000, .task_core = 1, \
        .task_prio = 10, .task_stack = 6144                                              \
    }

/* Start st_task: init the PHY on task_core, route TX, run. Call once, after
 * st_phy_tx_idle() at the top of app_main. */
esp_err_t st_core_start(const st_core_config_t *cfg);

/* RX callback, called from st_task for every link event:
 *  ev == ST_LINK_DATAGRAM: a valid foreign datagram; ds/dec hold the decode
 *        result, forward says whether the type's mode sends it to Signal K;
 *  ev == ST_LINK_FLAGGED / ST_LINK_INCOMPLETE: diagnostics only, dec is NULL,
 *        forward is false. Never decode these.
 * Keep it short: it runs on the bus task. */
typedef void (*st_rx_cb_t)(st_link_event_t ev, const st_datagram_t *dg, st_dec_status_t ds,
                           const st_decoded_t *dec, bool forward, void *arg);
#define ST_CORE_MAX_RX_CB 4
/* Add a listener (bench monitor, Signal K adapter, $STALK ...), called in
 * registration order. ESP_ERR_NO_MEM when ST_CORE_MAX_RX_CB are registered. */
esp_err_t st_core_add_rx(st_rx_cb_t cb, void *arg);

/* Value for the bus direction, timestamped now. NaN clears it. */
esp_err_t st_core_set_value(st_quantity_t q, double value);

esp_err_t st_core_set_dgram_cfg(uint8_t cmd, const st_dgram_cfg_t *cfg);
esp_err_t st_core_get_dgram_cfg(uint8_t cmd, st_dgram_cfg_t *cfg);
/* Copy of one slot (config, state, per-type stats). */
esp_err_t st_core_get_slot(uint8_t cmd, st_slot_t *out);
double st_core_planned_rate(void);

/* Queue one datagram (e.g. autopilot key 0x86 at HIGH, $STALK at NORMAL).
 * Sent politely, before periodic slots of the same or lower priority, with
 * the same retry rules. ESP_ERR_NO_MEM when the queue is full. */
esp_err_t st_core_send_once(const uint8_t *b, uint8_t len, st_prio_t prio);

/* Stop / resume periodic sending (one-offs and RX keep running). */
void st_core_pause(bool pause);
bool st_core_paused(void);

/* Change the idle wait at run time (spec: measure other devices first). */
void st_core_set_idle(uint32_t idle_us, uint32_t jitter_us);

typedef struct {
    uint32_t tx_ok;
    uint32_t tx_retried_ok;
    uint32_t tx_collisions;
    uint32_t tx_no_echo;
    uint32_t tx_bus_busy;    /* no quiet gap within ST_CORE_QUIET_TIMEOUT_MS */
    uint32_t tx_dropped;
    uint32_t stale_skips;
    uint32_t native_pauses;
    uint32_t once_sent;
    uint32_t once_dropped;
    uint32_t once_queue_full;
    uint32_t idle_wait_max_us; /* longest wait for a quiet gap */
    bool bus_held_low;
    bool no_echo_pause;
    bool paused;
    uint32_t own_bytes;      /* bus load accounting since reset */
    uint32_t foreign_bytes;
    int64_t since_us;        /* esp_timer time of the last reset */
    st_link_stats_t link;
    st_phy_stats_t phy;
} st_core_stats_t;

void st_core_get_stats(st_core_stats_t *out);
void st_core_reset_stats(void);

/* idle-before diagnostics per foreign datagram type (spec 9): count and
 * minimum quiet gap before that device started talking. */
void st_core_idle_before(uint8_t cmd, uint32_t *count, uint32_t *min_us, uint32_t *max_us);

/* Bench self-test (spec 10 test 1): 16 datagrams of 18 bytes covering every
 * byte value, command 0xF0, sent politely. Runs inside st_task with periodic
 * sending paused; blocks the caller until done. */
typedef struct {
    uint8_t ok, collision, no_echo, busy;
    uint32_t echo_bad, ambiguous, frame_err;
    bool pass;
} st_selftest_result_t;
esp_err_t st_core_selftest(st_selftest_result_t *out);

#ifdef __cplusplus
}
#endif
