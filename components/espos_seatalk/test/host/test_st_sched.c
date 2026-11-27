/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 */
/* Host unit tests for the SeaTalk 1 scheduler logic. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "st_sched.h"
#include "test_util.h"

#define CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                         \
        const double va_ = (double)(a);                                          \
        const double vb_ = (double)(b);                                          \
        if (!(fabs(va_ - vb_) <= (double)(eps))) {                               \
            printf("%s:%d: CHECK_NEAR failed: %s ~ %s\n    expected %.9g, actual %.9g (eps %g)\n", \
                   __FILE__, __LINE__, #a, #b, vb_, va_, (double)(eps));         \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

#define MAX_LOG 4096

typedef struct {
    int64_t t[MAX_LOG];
    uint8_t cmd[MAX_LOG];
    int n;
} send_log_t;

static send_log_t g_log;

static void set_all_values(st_sched_t *s, int64_t t)
{
    st_sched_set_value(s, ST_Q_DEPTH_BELOW_TRANSDUCER, 5.0, t);
    st_sched_set_value(s, ST_Q_WIND_ANGLE_APPARENT, 0.5, t);
    st_sched_set_value(s, ST_Q_WIND_SPEED_APPARENT, 5.0, t);
    st_sched_set_value(s, ST_Q_SPEED_THROUGH_WATER, 3.0, t);
    st_sched_set_value(s, ST_Q_TRIP_LOG, 1000.0, t);
    st_sched_set_value(s, ST_Q_TOTAL_LOG, 50000.0, t);
    st_sched_set_value(s, ST_Q_WATER_TEMPERATURE, 290.0, t);
    st_sched_set_value(s, ST_Q_SPEED_OVER_GROUND, 3.0, t);
    st_sched_set_value(s, ST_Q_COURSE_OVER_GROUND_MAGNETIC, 1.0, t);
    st_sched_set_value(s, ST_Q_HEADING_MAGNETIC, 2.0, t);
    st_sched_set_value(s, ST_Q_RUDDER_ANGLE, 0.1, t);
}

/* Run t in [t0, t1) in 1 ms steps; every returned datagram is logged and
 * completed with OK. refresh: re-set all values every ms. Also checks that
 * a slot is never returned twice in one ms. */
static void sim(st_sched_t *s, int64_t t0, int64_t t1, bool refresh)
{
    int64_t t;
    g_log.n = 0;
    for (t = t0; t < t1; t++) {
        uint8_t last[ST_SCHED_MAX_SLOTS];
        int nlast = 0;
        if (refresh) set_all_values(s, t);
        for (;;) {
            uint8_t buf[24], cmd = 0;
            uint8_t len = st_sched_next(s, t, buf, &cmd, NULL);
            int i;
            if (len == 0) break;
            for (i = 0; i < nlast; i++) CHECK(last[i] != cmd);
            if (nlast < ST_SCHED_MAX_SLOTS) last[nlast++] = cmd;
            if (g_log.n < MAX_LOG) {
                g_log.t[g_log.n] = t;
                g_log.cmd[g_log.n] = cmd;
                g_log.n++;
            }
            st_sched_done(s, cmd, ST_TX_OK, t);
            if (nlast >= ST_SCHED_MAX_SLOTS) break;
        }
    }
}

static int log_count(uint8_t cmd, int64_t from, int64_t to)
{
    int i, c = 0;
    for (i = 0; i < g_log.n; i++) {
        if (g_log.cmd[i] == cmd && g_log.t[i] >= from && g_log.t[i] < to) c++;
    }
    return c;
}

/* Everything OFF except 'cmd' (keeps its default config). */
static void isolate(st_sched_t *s, uint8_t cmd)
{
    uint8_t i;
    for (i = 0; i < s->n_slots; i++) {
        if (s->slot[i].cmd != cmd) {
            st_dgram_cfg_t c = s->slot[i].cfg;
            c.mode = ST_MODE_OFF;
            CHECK(st_sched_set_cfg(s, s->slot[i].cmd, &c, 0));
        }
    }
}

static void test_init(void)
{
    st_sched_t s;
    uint8_t n = 0;
    const st_dgram_info_t *tab = st_codec_table(&n);
    uint8_t i;
    st_sched_init(&s, 0);
    CHECK_EQ_INT(s.n_slots, n);
    for (i = 0; i < n; i++) {
        CHECK_EQ_INT(s.slot[i].cmd, tab[i].cmd);
        CHECK_EQ_INT(s.slot[i].cfg.mode, ST_MODE_AUTO);
        CHECK_EQ_INT(s.slot[i].cfg.period_ms, tab[i].default_period_ms);
        CHECK_EQ_INT(s.slot[i].cfg.stale_ms, ST_SCHED_DEFAULT_STALE_MS);
        CHECK(s.slot[i].native_seen_ms == INT64_MIN);
    }
    CHECK_EQ_INT(st_sched_slot(&s, 0x00)->cfg.period_ms, 1000);
    CHECK_EQ_INT(st_sched_slot(&s, 0x10)->cfg.period_ms, 500);
    CHECK_EQ_INT(st_sched_slot(&s, 0x25)->cfg.period_ms, 5000);
    CHECK_EQ_INT(st_sched_slot(&s, 0x21)->cfg.period_ms, 0);
    CHECK_EQ_INT(st_sched_slot(&s, 0x25)->prio, ST_PRIO_LOW);
    CHECK_EQ_INT(st_sched_slot(&s, 0x27)->prio, ST_PRIO_LOW);
    CHECK_EQ_INT(st_sched_slot(&s, 0x00)->prio, ST_PRIO_NORMAL);
    CHECK(st_sched_slot(&s, 0x77) == NULL);
    CHECK_NEAR(st_sched_planned_rate(&s), 9.4, 1e-9);
}

static void test_nothing_due_early_and_rates(void)
{
    st_sched_t s;
    uint8_t buf[24], cmd;
    uint8_t i;
    int total = 0;
    st_sched_init(&s, 0);
    set_all_values(&s, 0);
    CHECK_EQ_INT(st_sched_next(&s, 0, buf, &cmd, NULL), 0);
    CHECK_EQ_INT(st_sched_next(&s, 40, buf, &cmd, NULL), 0);

    sim(&s, 0, 60000, true);
    for (i = 0; i < s.n_slots; i++) {
        uint32_t p = s.slot[i].cfg.period_ms;
        int c = log_count(s.slot[i].cmd, 0, 60000);
        total += c;
        if (p) {
            int expect = (int)(60000 / p);
            CHECK(c >= expect - 1 && c <= expect + 1);
            CHECK_EQ_INT(s.slot[i].st.sent, c);
        } else {
            CHECK_EQ_INT(c, 0);
        }
    }
    CHECK(fabs((double)total - 9.4 * 60.0) <= 12.0);
    CHECK_EQ_INT(g_log.n, total);
}

static void test_priority(void)
{
    st_sched_t s;
    uint8_t buf[24], cmd = 0;
    st_sched_init(&s, 0);
    set_all_values(&s, 100);
    isolate(&s, 0x00);
    /* NORMAL (0x00) vs LOW (0x27), LOW is due earlier */
    {
        st_dgram_cfg_t c = *(&st_sched_slot(&s, 0x27)->cfg);
        c.mode = ST_MODE_AUTO;
        CHECK(st_sched_set_cfg(&s, 0x27, &c, 0));
    }
    ((st_slot_t *)st_sched_slot(&s, 0x00))->due_ms = 100;
    ((st_slot_t *)st_sched_slot(&s, 0x27))->due_ms = 50;
    CHECK(st_sched_next(&s, 100, buf, &cmd, NULL) > 0);
    CHECK_EQ_INT(cmd, 0x00);
    st_sched_done(&s, cmd, ST_TX_OK, 100);
    CHECK(st_sched_next(&s, 100, buf, &cmd, NULL) > 0);
    CHECK_EQ_INT(cmd, 0x27);
    st_sched_done(&s, cmd, ST_TX_OK, 100);
    CHECK_EQ_INT(st_sched_next(&s, 100, buf, &cmd, NULL), 0);

    /* equal priority: earlier due first */
    {
        st_dgram_cfg_t c = st_sched_slot(&s, 0x10)->cfg;
        c.mode = ST_MODE_AUTO;
        CHECK(st_sched_set_cfg(&s, 0x10, &c, 0));
    }
    ((st_slot_t *)st_sched_slot(&s, 0x00))->due_ms = 200;
    ((st_slot_t *)st_sched_slot(&s, 0x10))->due_ms = 150;
    set_all_values(&s, 300);
    CHECK(st_sched_next(&s, 300, buf, &cmd, NULL) > 0);
    CHECK_EQ_INT(cmd, 0x10);
    st_sched_done(&s, cmd, ST_TX_OK, 300);
    CHECK(st_sched_next(&s, 300, buf, &cmd, NULL) > 0);
    CHECK_EQ_INT(cmd, 0x00);
}

static void test_stale(void)
{
    st_sched_t s;
    int64_t t;
    int sent_before;
    st_sched_init(&s, 0);
    isolate(&s, 0x00);
    st_sched_set_value(&s, ST_Q_DEPTH_BELOW_TRANSDUCER, 4.0, 0);

    sim(&s, 0, 5001, false);
    CHECK_EQ_INT(log_count(0x00, 0, 5001), 5);
    CHECK_EQ_INT(st_sched_slot(&s, 0x00)->st.stale_skips, 0);

    sim(&s, 5001, 9000, false);
    CHECK_EQ_INT(g_log.n, 0);
    CHECK(st_sched_slot(&s, 0x00)->st.stale_skips >= 3);

    st_sched_set_value(&s, ST_Q_DEPTH_BELOW_TRANSDUCER, 4.0, 9000);
    sent_before = (int)st_sched_slot(&s, 0x00)->st.sent;
    for (t = 9000; t < 10000; t++) {
        uint8_t buf[24], cmd;
        if (st_sched_next(&s, t, buf, &cmd, NULL)) st_sched_done(&s, cmd, ST_TX_OK, t);
    }
    CHECK_EQ_INT(st_sched_slot(&s, 0x00)->st.sent, sent_before + 1);

    /* missing value: never sent */
    st_sched_init(&s, 0);
    isolate(&s, 0x00);
    sim(&s, 0, 4000, false);
    CHECK_EQ_INT(g_log.n, 0);
    CHECK_EQ_INT(st_sched_slot(&s, 0x00)->st.stale_skips, 4);

    /* 25 needs both total and trip */
    st_sched_init(&s, 0);
    isolate(&s, 0x25);
    st_sched_set_value(&s, ST_Q_TOTAL_LOG, 1000.0, 0);
    sim(&s, 0, 4000, false);
    CHECK_EQ_INT(g_log.n, 0);
    CHECK(st_sched_slot(&s, 0x25)->st.stale_skips >= 1);
    st_sched_set_value(&s, ST_Q_TRIP_LOG, 10.0, 4000);
    st_sched_set_value(&s, ST_Q_TOTAL_LOG, 1000.0, 4000);
    sim(&s, 4000, 12000, false);
    CHECK(log_count(0x25, 4000, 12000) >= 1);
}

static void test_native_hold(void)
{
    st_sched_t s;
    const st_slot_t *d;
    st_dgram_cfg_t c;
    st_sched_init(&s, 0);
    isolate(&s, 0x00);
    d = st_sched_slot(&s, 0x00);

    sim(&s, 0, 3000, true);
    CHECK(log_count(0x00, 0, 3000) >= 2);
    CHECK(st_sched_on_foreign(&s, 0x00, 3000));
    CHECK(st_sched_native_active(&s, 0x00, 3000));
    CHECK(st_sched_native_active(&s, 0x00, 12999));
    CHECK(!st_sched_native_active(&s, 0x00, 13000));
    sim(&s, 3000, 8000, true);
    CHECK_EQ_INT(g_log.n, 0);
    CHECK(d->st.native_pauses > 0);
    /* a new foreign datagram extends the hold */
    CHECK(st_sched_on_foreign(&s, 0x00, 8000));
    sim(&s, 8000, 18000, true);
    CHECK_EQ_INT(g_log.n, 0);
    sim(&s, 18000, 21000, true);
    CHECK(g_log.n >= 2);
    CHECK_EQ_INT(d->st.rx_foreign, 2);

    /* BOTH ignores foreign datagrams */
    c = d->cfg;
    c.mode = ST_MODE_BOTH;
    CHECK(st_sched_set_cfg(&s, 0x00, &c, 21000));
    CHECK(st_sched_on_foreign(&s, 0x00, 21000));
    sim(&s, 21000, 24000, true);
    CHECK(g_log.n >= 2);

    /* TO_SK never sends, forwards */
    c.mode = ST_MODE_TO_SK;
    CHECK(st_sched_set_cfg(&s, 0x00, &c, 24000));
    CHECK(st_sched_on_foreign(&s, 0x00, 24000));
    sim(&s, 24000, 30000, true);
    CHECK_EQ_INT(g_log.n, 0);

    /* OFF never sends, no forward */
    c.mode = ST_MODE_OFF;
    CHECK(st_sched_set_cfg(&s, 0x00, &c, 30000));
    CHECK(!st_sched_on_foreign(&s, 0x00, 30000));
    sim(&s, 30000, 34000, true);
    CHECK_EQ_INT(g_log.n, 0);

    /* TO_BUS: not forwarded */
    c.mode = ST_MODE_TO_BUS;
    CHECK(st_sched_set_cfg(&s, 0x00, &c, 34000));
    CHECK(!st_sched_on_foreign(&s, 0x00, 34000));

    /* unknown type */
    CHECK(!st_sched_on_foreign(&s, 0x77, 34000));
}

static void test_retries(void)
{
    st_sched_t s;
    uint8_t buf[24], cmd = 0;
    const st_slot_t *d;
    int64_t due;
    int i;
    st_sched_init(&s, 0);
    isolate(&s, 0x00);
    set_all_values(&s, 0);
    d = st_sched_slot(&s, 0x00);
    due = d->due_ms;

    CHECK(st_sched_next(&s, due, buf, &cmd, NULL) > 0);
    CHECK_EQ_INT(st_sched_retries(&s, 0x00), 0);
    for (i = 1; i <= ST_SCHED_MAX_RETRIES; i++) {
        st_sched_done(&s, 0x00, ST_TX_COLLISION, due);
        CHECK_EQ_INT(st_sched_retries(&s, 0x00), i);
        CHECK_EQ_INT(d->due_ms, due);
        CHECK(st_sched_next(&s, due, buf, &cmd, NULL) > 0);
        CHECK_EQ_INT(cmd, 0x00);
    }
    CHECK_EQ_INT(d->st.collisions, ST_SCHED_MAX_RETRIES);
    CHECK_EQ_INT(d->st.dropped, 0);
    st_sched_done(&s, 0x00, ST_TX_COLLISION, due);
    CHECK_EQ_INT(d->st.dropped, 1);
    CHECK_EQ_INT(d->st.collisions, ST_SCHED_MAX_RETRIES + 1);
    CHECK_EQ_INT(st_sched_retries(&s, 0x00), 0);
    CHECK_EQ_INT(d->due_ms, due + 1000);
    CHECK_EQ_INT(st_sched_next(&s, due, buf, &cmd, NULL), 0);

    /* success after 2 collisions */
    due = d->due_ms;
    set_all_values(&s, due);
    CHECK(st_sched_next(&s, due, buf, &cmd, NULL) > 0);
    st_sched_done(&s, 0x00, ST_TX_COLLISION, due);
    st_sched_done(&s, 0x00, ST_TX_COLLISION, due);
    CHECK_EQ_INT(st_sched_retries(&s, 0x00), 2);
    st_sched_done(&s, 0x00, ST_TX_OK, due);
    CHECK_EQ_INT(d->st.sent, 1);
    CHECK_EQ_INT(d->st.retried_ok, 1);
    CHECK_EQ_INT(st_sched_retries(&s, 0x00), 0);
    CHECK_EQ_INT(d->due_ms, due + 1000);

    /* NO_ECHO and NOT_READY: dropped, next period */
    due = d->due_ms;
    set_all_values(&s, due);
    CHECK(st_sched_next(&s, due, buf, &cmd, NULL) > 0);
    st_sched_done(&s, 0x00, ST_TX_NO_ECHO, due);
    CHECK_EQ_INT(d->st.dropped, 2);
    CHECK_EQ_INT(d->due_ms, due + 1000);
    due = d->due_ms;
    set_all_values(&s, due);
    CHECK(st_sched_next(&s, due, buf, &cmd, NULL) > 0);
    st_sched_done(&s, 0x00, ST_TX_NOT_READY, due);
    CHECK_EQ_INT(d->st.dropped, 3);
    CHECK_EQ_INT(d->due_ms, due + 1000);

    /* unknown cmd is ignored */
    st_sched_done(&s, 0x77, ST_TX_OK, due);
    CHECK_EQ_INT(d->st.sent, 1);
}

static void test_no_catch_up(void)
{
    st_sched_t s;
    uint8_t buf[24], cmd = 0;
    const st_slot_t *d;
    int64_t due, now;
    st_sched_init(&s, 0);
    isolate(&s, 0x00);
    d = st_sched_slot(&s, 0x00);
    due = d->due_ms;
    set_all_values(&s, due);
    CHECK(st_sched_next(&s, due, buf, &cmd, NULL) > 0);
    now = due + 10000;
    st_sched_done(&s, 0x00, ST_TX_OK, now);
    CHECK_EQ_INT(d->due_ms, now + 1000);
    set_all_values(&s, now);
    CHECK_EQ_INT(st_sched_next(&s, now, buf, &cmd, NULL), 0);
    CHECK_EQ_INT(st_sched_next(&s, now + 999, buf, &cmd, NULL), 0);
    CHECK(st_sched_next(&s, now + 1000, buf, &cmd, NULL) > 0);
}

static void test_set_cfg(void)
{
    st_sched_t s;
    st_dgram_cfg_t c, g;
    st_mode_t m;
    int i;
    const char *names[] = {"off", "to_sk", "to_bus", "both", "auto"};

    st_sched_init(&s, 0);
    c.mode = ST_MODE_AUTO;
    c.period_ms = 1000;
    c.stale_ms = 5000;
    CHECK(!st_sched_set_cfg(&s, 0x77, &c, 0));
    c.mode = (st_mode_t)9;
    CHECK(!st_sched_set_cfg(&s, 0x00, &c, 0));
    c.mode = ST_MODE_TO_BUS;
    CHECK(!st_sched_set_cfg(&s, 0x30, &c, 0));
    c.mode = ST_MODE_TO_SK;
    CHECK(st_sched_set_cfg(&s, 0x30, &c, 0));
    CHECK(st_sched_get_cfg(&s, 0x30, &g));
    CHECK_EQ_INT(g.mode, ST_MODE_TO_SK);
    CHECK_EQ_INT(g.period_ms, 1000);
    c.mode = ST_MODE_TO_BUS;
    c.period_ms = 0;
    CHECK(st_sched_set_cfg(&s, 0x30, &c, 0));
    CHECK(!st_sched_get_cfg(&s, 0x77, &g));

    /* prio follows the period */
    st_sched_init(&s, 0);
    c.mode = ST_MODE_AUTO;
    c.stale_ms = 5000;
    c.period_ms = 6000;
    CHECK(st_sched_set_cfg(&s, 0x00, &c, 0));
    CHECK_EQ_INT(st_sched_slot(&s, 0x00)->prio, ST_PRIO_LOW);
    c.period_ms = 1000;
    CHECK(st_sched_set_cfg(&s, 0x00, &c, 0));
    CHECK_EQ_INT(st_sched_slot(&s, 0x00)->prio, ST_PRIO_NORMAL);

    /* 0x10: 500 ms -> 2000 ms */
    st_sched_init(&s, 0);
    sim(&s, 0, 60000, true);
    {
        int before = log_count(0x10, 0, 60000);
        CHECK(before >= 119 && before <= 121);
        st_sched_init(&s, 0);
        c.period_ms = 2000;
        CHECK(st_sched_set_cfg(&s, 0x10, &c, 0));
        CHECK_NEAR(st_sched_planned_rate(&s), 9.4 - 2.0 + 0.5, 1e-9);
        sim(&s, 0, 60000, true);
        {
            int after = log_count(0x10, 0, 60000);
            CHECK(after >= 29 && after <= 31);
        }
    }

    for (i = 0; i < 5; i++) {
        CHECK(strcmp(st_mode_name((st_mode_t)i), names[i]) == 0);
        CHECK(st_mode_from_name(names[i], &m));
        CHECK_EQ_INT(m, i);
    }
    CHECK(!st_mode_from_name("bogus", &m));
    CHECK(!st_mode_from_name(NULL, &m));
    CHECK(strcmp(st_mode_name((st_mode_t)42), "?") == 0);
}

static void test_next_due(void)
{
    st_sched_t s;
    uint8_t buf[24], cmd;
    int64_t nd = 0, min_due = INT64_MAX;
    uint8_t i;
    st_sched_init(&s, 0);
    set_all_values(&s, 0);
    CHECK_EQ_INT(st_sched_next(&s, 0, buf, &cmd, &nd), 0);
    for (i = 0; i < s.n_slots; i++) {
        if (s.slot[i].cfg.period_ms && s.slot[i].due_ms < min_due) min_due = s.slot[i].due_ms;
    }
    CHECK(nd == min_due);
    CHECK(nd > 0 && nd < 500);

    /* after serving the first, the next one is reported */
    CHECK(st_sched_next(&s, nd, buf, &cmd, &nd) > 0);
    CHECK(nd <= 500);
    st_sched_done(&s, cmd, ST_TX_OK, min_due);

    /* all OFF */
    for (i = 0; i < s.n_slots; i++) {
        st_dgram_cfg_t c = s.slot[i].cfg;
        c.mode = ST_MODE_OFF;
        CHECK(st_sched_set_cfg(&s, s.slot[i].cmd, &c, 0));
    }
    CHECK_EQ_INT(st_sched_next(&s, 100000, buf, &cmd, &nd), 0);
    CHECK(nd == INT64_MAX);
    CHECK_NEAR(st_sched_planned_rate(&s), 0.0, 1e-12);

    /* stale skip moves due before next_due is computed */
    st_sched_init(&s, 0);
    isolate(&s, 0x00);
    nd = 0;
    CHECK_EQ_INT(st_sched_next(&s, 5000, buf, &cmd, &nd), 0);
    CHECK(nd > 5000);
    CHECK(st_sched_slot(&s, 0x00)->st.stale_skips == 1);
}

static void test_heading_only(void)
{
    st_sched_t s;
    uint8_t buf[24], cmd = 0;
    int64_t due;
    st_sched_init(&s, 0);
    {
        /* 9C is off by default (the ST50 reads heading from 89): enable it. */
        const st_dgram_cfg_t on = {ST_MODE_AUTO, 1000, ST_SCHED_DEFAULT_STALE_MS};
        CHECK(st_sched_set_cfg(&s, 0x9C, &on, 0));
    }
    isolate(&s, 0x9C);
    st_sched_set_value(&s, ST_Q_HEADING_MAGNETIC, 1.0, 0);
    due = st_sched_slot(&s, 0x9C)->due_ms;
    CHECK_EQ_INT(st_sched_next(&s, due, buf, &cmd, NULL), 4);
    CHECK_EQ_INT(cmd, 0x9C);
    CHECK_EQ_INT(buf[0], 0x9C);
}

int main(void)
{
    RUN(test_init);
    RUN(test_nothing_due_early_and_rates);
    RUN(test_priority);
    RUN(test_stale);
    RUN(test_native_hold);
    RUN(test_retries);
    RUN(test_no_catch_up);
    RUN(test_set_cfg);
    RUN(test_next_due);
    RUN(test_heading_only);
    return test_report();
}
