/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 */
/* SeaTalk 1 scheduler logic: see include/st_sched.h. Pure C, static storage. */
#include "st_sched.h"

#include <string.h>

#define PRIO_LOW_PERIOD_MS 5000u

typedef struct {
    const st_sched_t *s;
    uint32_t stale_ms;
} get_ctx_t;

static bool value_get(st_quantity_t q, double *value, void *arg)
{
    const get_ctx_t *c = (const get_ctx_t *)arg;
    const st_value_slot_t *v;
    if ((int)q <= 0 || q >= ST_Q_COUNT) return false;
    v = &c->s->val[q];
    if (!v->has) return false;
    if ((c->s->now_ms - v->t_ms) > (int64_t)c->stale_ms) return false;
    *value = v->value;
    return true;
}

static st_prio_t prio_for(uint32_t period_ms)
{
    return period_ms >= PRIO_LOW_PERIOD_MS ? ST_PRIO_LOW : ST_PRIO_NORMAL;
}

static bool mode_valid(st_mode_t m)
{
    return (int)m >= (int)ST_MODE_OFF && (int)m <= (int)ST_MODE_AUTO;
}

static bool mode_to_bus(st_mode_t m)
{
    return m == ST_MODE_TO_BUS || m == ST_MODE_BOTH || m == ST_MODE_AUTO;
}

static st_slot_t *find_slot(st_sched_t *s, uint8_t cmd)
{
    uint8_t i;
    for (i = 0; i < s->n_slots; i++) {
        if (s->slot[i].cmd == cmd) return &s->slot[i];
    }
    return NULL;
}

/* A slot that may transmit at all (ignoring native hold and values). */
static bool slot_sendable(const st_slot_t *sl)
{
    return sl->cfg.period_ms > 0 && sl->info->can_encode && mode_to_bus(sl->cfg.mode);
}

static bool native_active_slot(const st_slot_t *sl, int64_t now_ms)
{
    if (sl->native_seen_ms == INT64_MIN) return false;
    if (now_ms < sl->native_seen_ms) return true;
    return (now_ms - sl->native_seen_ms) < (int64_t)ST_SCHED_NATIVE_HOLD_MS;
}

/* Move due one period on; never leave it in the past (no catch-up burst). */
static void next_period(st_slot_t *sl, int64_t now_ms)
{
    sl->due_ms += (int64_t)sl->cfg.period_ms;
    if (sl->due_ms <= now_ms) sl->due_ms = now_ms + (int64_t)sl->cfg.period_ms;
}

void st_sched_init(st_sched_t *s, int64_t now_ms)
{
    uint8_t n = 0;
    uint8_t i;
    const st_dgram_info_t *tab = st_codec_table(&n);

    memset(s, 0, sizeof(*s));
    if (n > ST_SCHED_MAX_SLOTS) n = ST_SCHED_MAX_SLOTS;
    s->n_slots = n;
    s->now_ms = now_ms;
    for (i = 0; i < n; i++) {
        st_slot_t *sl = &s->slot[i];
        uint32_t p = tab[i].default_period_ms;
        sl->cmd = tab[i].cmd;
        sl->info = &tab[i];
        sl->cfg.mode = ST_MODE_AUTO;
        sl->cfg.period_ms = p;
        sl->cfg.stale_ms = ST_SCHED_DEFAULT_STALE_MS;
        sl->prio = prio_for(p);
        /* Stagger: slot i first fires at now + period * (i+1) / (n+1), so
         * the slots spread over (0, period) and never all fire together. */
        sl->due_ms = now_ms + (int64_t)(((uint64_t)p * (uint64_t)(i + 1)) / (uint64_t)(n + 1));
        sl->native_seen_ms = INT64_MIN;
    }
}

bool st_sched_set_cfg(st_sched_t *s, uint8_t cmd, const st_dgram_cfg_t *cfg, int64_t now_ms)
{
    st_slot_t *sl = find_slot(s, cmd);
    if (!sl || !cfg || !mode_valid(cfg->mode)) return false;
    if (!sl->info->can_encode && mode_to_bus(cfg->mode) && cfg->period_ms > 0) return false;

    if (cfg->period_ms != sl->cfg.period_ms) {
        int64_t fresh = now_ms + (int64_t)cfg->period_ms;
        if (sl->cfg.period_ms == 0 || fresh < sl->due_ms) sl->due_ms = fresh;
        sl->prio = prio_for(cfg->period_ms);
    }
    sl->cfg = *cfg;
    return true;
}

bool st_sched_get_cfg(const st_sched_t *s, uint8_t cmd, st_dgram_cfg_t *cfg)
{
    const st_slot_t *sl = st_sched_slot(s, cmd);
    if (!sl || !cfg) return false;
    *cfg = sl->cfg;
    return true;
}

void st_sched_set_value(st_sched_t *s, st_quantity_t q, double value, int64_t now_ms)
{
    if ((int)q <= 0 || q >= ST_Q_COUNT) return;
    s->val[q].has = true;
    s->val[q].value = value;
    s->val[q].t_ms = now_ms;
}

void st_sched_clear_value(st_sched_t *s, st_quantity_t q)
{
    if ((int)q <= 0 || q >= ST_Q_COUNT) return;
    memset(&s->val[q], 0, sizeof(s->val[q]));
}

bool st_sched_on_foreign(st_sched_t *s, uint8_t cmd, int64_t now_ms)
{
    st_slot_t *sl = find_slot(s, cmd);
    if (!sl) return false;
    sl->native_seen_ms = now_ms;
    sl->st.rx_foreign++;
    return sl->cfg.mode == ST_MODE_TO_SK || sl->cfg.mode == ST_MODE_BOTH || sl->cfg.mode == ST_MODE_AUTO;
}

bool st_sched_native_active(const st_sched_t *s, uint8_t cmd, int64_t now_ms)
{
    const st_slot_t *sl = st_sched_slot(s, cmd);
    return sl && native_active_slot(sl, now_ms);
}

uint8_t st_sched_next(st_sched_t *s, int64_t now_ms, uint8_t *out, uint8_t *cmd, int64_t *next_due_ms)
{
    uint8_t result = 0;
    uint8_t i;

    s->now_ms = now_ms;
    for (;;) {
        st_slot_t *best = NULL;
        get_ctx_t ctx;
        uint8_t len;

        for (i = 0; i < s->n_slots; i++) {
            st_slot_t *sl = &s->slot[i];
            if (!slot_sendable(sl) || sl->due_ms > now_ms) continue;
            if (!best || sl->prio > best->prio || (sl->prio == best->prio && sl->due_ms < best->due_ms)) best = sl;
        }
        if (!best) break;

        if (best->cfg.mode == ST_MODE_AUTO && native_active_slot(best, now_ms)) {
            best->st.native_pauses++;
            next_period(best, now_ms);
            continue;
        }

        ctx.s = s;
        ctx.stale_ms = best->cfg.stale_ms;
        len = st_codec_encode(best->cmd, value_get, &ctx, out);
        if (len == 0) {
            best->st.stale_skips++;
            next_period(best, now_ms);
            continue;
        }
        if (cmd) *cmd = best->cmd;
        result = len;
        break;
    }

    if (next_due_ms) {
        int64_t min_due = INT64_MAX;
        for (i = 0; i < s->n_slots; i++) {
            const st_slot_t *sl = &s->slot[i];
            if (slot_sendable(sl) && sl->due_ms < min_due) min_due = sl->due_ms;
        }
        *next_due_ms = min_due;
    }
    return result;
}

uint8_t st_sched_retries(const st_sched_t *s, uint8_t cmd)
{
    const st_slot_t *sl = st_sched_slot(s, cmd);
    return sl ? sl->retries : 0;
}

void st_sched_done(st_sched_t *s, uint8_t cmd, st_tx_result_t r, int64_t now_ms)
{
    st_slot_t *sl = find_slot(s, cmd);
    if (!sl) return;

    switch (r) {
    case ST_TX_OK:
        sl->st.sent++;
        if (sl->retries > 0) sl->st.retried_ok++;
        sl->retries = 0;
        next_period(sl, now_ms);
        break;
    case ST_TX_COLLISION:
        sl->st.collisions++;
        if (sl->retries >= ST_SCHED_MAX_RETRIES) {
            sl->st.dropped++;
            sl->retries = 0;
            next_period(sl, now_ms);
        } else {
            sl->retries++;
        }
        break;
    default: /* NO_ECHO, NOT_READY: no retry */
        sl->st.dropped++;
        sl->retries = 0;
        next_period(sl, now_ms);
        break;
    }
}

const st_slot_t *st_sched_slot(const st_sched_t *s, uint8_t cmd)
{
    uint8_t i;
    for (i = 0; i < s->n_slots; i++) {
        if (s->slot[i].cmd == cmd) return &s->slot[i];
    }
    return NULL;
}

double st_sched_planned_rate(const st_sched_t *s)
{
    double rate = 0.0;
    uint8_t i;
    for (i = 0; i < s->n_slots; i++) {
        const st_slot_t *sl = &s->slot[i];
        if (slot_sendable(sl)) rate += 1000.0 / (double)sl->cfg.period_ms;
    }
    return rate;
}

void st_sched_reset_stats(st_sched_t *s)
{
    uint8_t i;
    for (i = 0; i < s->n_slots; i++) memset(&s->slot[i].st, 0, sizeof(s->slot[i].st));
}

static const char *const k_mode_names[] = {"off", "to_sk", "to_bus", "both", "auto"};

const char *st_mode_name(st_mode_t m)
{
    if (!mode_valid(m)) return "?";
    return k_mode_names[(int)m];
}

bool st_mode_from_name(const char *name, st_mode_t *out)
{
    int i;
    if (!name) return false;
    for (i = 0; i < 5; i++) {
        if (strcmp(name, k_mode_names[i]) == 0) {
            if (out) *out = (st_mode_t)i;
            return true;
        }
    }
    return false;
}
