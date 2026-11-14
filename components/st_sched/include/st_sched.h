/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * SeaTalk 1 scheduler logic (spec section 5.2): what to send next.
 *
 * Pure C, no hardware, no RTOS, time passed in: unit-tested on a PC. Not
 * thread-safe; st_core serialises access. It decides *what* and *when*;
 * st_core does the bus access (polite idle wait, send, back-off sleep).
 *
 * One slot per v1 datagram type from st_codec's table. Per slot:
 *  - mode (spec 7.1):
 *      OFF     nothing in either direction
 *      TO_SK   bus -> Signal K only
 *      TO_BUS  Signal K -> bus only
 *      BOTH    both directions, no native-source check
 *      AUTO    bus -> Signal K always; Signal K -> bus only while no other
 *              bus device has sent this type in the last ST_SCHED_NATIVE_HOLD_MS
 *  - period_ms: transmit period (0 = never sent)
 *  - stale_ms: a value older than this is not sent; the slot stays silent
 *    and the display goes blank (rule 7)
 *  - priority: LOW for period >= 5000 ms, else NORMAL (HIGH is for one-off
 *    datagrams such as autopilot keys, queued in st_core)
 *
 * Values to send come from the value store (st_sched_set_value), fed by an
 * adapter (Signal K, NMEA 0183, bench). Encoding uses st_codec.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "st_codec.h"
#include "st_phy_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_SCHED_NATIVE_HOLD_MS 10000 /* rule 8 */
#define ST_SCHED_MAX_RETRIES 5        /* rule 2 */
#define ST_SCHED_DEFAULT_STALE_MS 5000
#define ST_SCHED_MAX_SLOTS 24

typedef enum {
    ST_MODE_OFF = 0,
    ST_MODE_TO_SK = 1,
    ST_MODE_TO_BUS = 2,
    ST_MODE_BOTH = 3,
    ST_MODE_AUTO = 4,
} st_mode_t;

typedef enum {
    ST_PRIO_LOW = 0,
    ST_PRIO_NORMAL = 1,
    ST_PRIO_HIGH = 2,
} st_prio_t;

typedef struct {
    st_mode_t mode;
    uint32_t period_ms;
    uint32_t stale_ms;
} st_dgram_cfg_t;

typedef struct {
    uint32_t sent;          /* datagrams sent ok */
    uint32_t retried_ok;    /* of which needed at least one retry */
    uint32_t collisions;    /* attempts that collided */
    uint32_t dropped;       /* given up after ST_SCHED_MAX_RETRIES, or no-echo */
    uint32_t stale_skips;   /* due, but a value was missing/stale: stayed silent */
    uint32_t native_pauses; /* due, but AUTO paused it for a native talker */
    uint32_t rx_foreign;    /* valid datagrams of this type from other devices */
} st_slot_stats_t;

typedef struct {
    uint8_t cmd;
    const st_dgram_info_t *info;
    st_dgram_cfg_t cfg;
    st_prio_t prio;
    int64_t due_ms;         /* next transmit time */
    int64_t native_seen_ms; /* last foreign datagram of this type; INT64_MIN = never */
    uint8_t retries;        /* collisions in a row for the current datagram */
    st_slot_stats_t st;
} st_slot_t;

typedef struct {
    bool has;
    double value;
    int64_t t_ms;
} st_value_slot_t;

typedef struct {
    st_slot_t slot[ST_SCHED_MAX_SLOTS];
    uint8_t n_slots;
    st_value_slot_t val[ST_Q_COUNT];
    int64_t now_ms; /* time of the current st_sched_next() call, for the getter */
} st_sched_t;

/* Slots from st_codec_table(): mode AUTO, period = default_period_ms,
 * stale = ST_SCHED_DEFAULT_STALE_MS. First due times are staggered over
 * each slot's period so the slots don't all fire together. */
void st_sched_init(st_sched_t *s, int64_t now_ms);

/* Per-type config. Returns false for an unknown cmd, a cmd that can't be
 * encoded with mode TO_BUS/BOTH/AUTO and period > 0, or an invalid mode.
 * Changing the period reschedules the slot within one new period. */
bool st_sched_set_cfg(st_sched_t *s, uint8_t cmd, const st_dgram_cfg_t *cfg, int64_t now_ms);
bool st_sched_get_cfg(const st_sched_t *s, uint8_t cmd, st_dgram_cfg_t *cfg);

/* Value store for the bus direction. */
void st_sched_set_value(st_sched_t *s, st_quantity_t q, double value, int64_t now_ms);
void st_sched_clear_value(st_sched_t *s, st_quantity_t q);

/* Call for every VALID foreign datagram (never our echo, never flagged).
 * Records the native talker for AUTO and counts it. Returns true when the
 * type's mode forwards bus -> Signal K (TO_SK, BOTH, AUTO). Unknown types
 * return false (the raw callback still sees them in st_core). */
bool st_sched_on_foreign(st_sched_t *s, uint8_t cmd, int64_t now_ms);

/* True while AUTO holds this type back for a native talker. */
bool st_sched_native_active(const st_sched_t *s, uint8_t cmd, int64_t now_ms);

/* Pick and encode the next datagram due at now_ms.
 *  - candidates: period > 0, due <= now, mode TO_BUS or BOTH, or AUTO
 *    without a native talker in the last ST_SCHED_NATIVE_HOLD_MS;
 *  - order: highest priority first, then earliest due, then table order;
 *  - an AUTO slot held back is counted (native_pauses) and its due moves one
 *    period on; a slot whose values are missing or older than stale_ms is
 *    counted (stale_skips) and its due moves one period on; both are skipped
 *    and the search continues.
 * Returns the encoded length (out >= 18 bytes) and the slot's cmd, or 0 when
 * nothing is due. *next_due_ms (may be NULL) gets the earliest due time of
 * any sendable slot after this call (INT64_MAX when there is none), so the
 * caller can sleep until then. The slot stays due until st_sched_done(). */
uint8_t st_sched_next(st_sched_t *s, int64_t now_ms, uint8_t *out, uint8_t *cmd, int64_t *next_due_ms);

/* Retries already made for cmd's current datagram (0 on the first attempt);
 * st_core uses it to choose the back-off. */
uint8_t st_sched_retries(const st_sched_t *s, uint8_t cmd);

/* Outcome of an attempt for cmd (from st_sched_next):
 *  ST_TX_OK        sent++ (retried_ok++ if retries > 0), retries = 0,
 *                  due += period; if that is already in the past,
 *                  due = now + period (no catch-up burst).
 *  ST_TX_COLLISION collisions++, retries++; stays due (st_core backs off
 *                  first); after ST_SCHED_MAX_RETRIES retries: dropped++,
 *                  retries = 0, next period.
 *  ST_TX_NO_ECHO,
 *  ST_TX_NOT_READY dropped++, retries = 0, next period (no retry: rule 3;
 *                  st_core pauses sending). */
void st_sched_done(st_sched_t *s, uint8_t cmd, st_tx_result_t r, int64_t now_ms);

/* Slot access for stats and config listings (NULL for an unknown cmd). */
const st_slot_t *st_sched_slot(const st_sched_t *s, uint8_t cmd);

/* Sum of 1000/period over slots that may send (mode != OFF/TO_SK, period > 0),
 * in datagrams per second. Spec 5.2 rule 4 plans for <= 10/s. */
double st_sched_planned_rate(const st_sched_t *s);

void st_sched_reset_stats(st_sched_t *s);

const char *st_mode_name(st_mode_t m);
/* "off", "to_sk", "to_bus", "both", "auto" -> mode; false if unknown. */
bool st_mode_from_name(const char *name, st_mode_t *out);

#ifdef __cplusplus
}
#endif
