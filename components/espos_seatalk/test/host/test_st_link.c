/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <string.h>

#include "st_link.h"
#include "test_util.h"

#define MAXEV 32

typedef struct {
    st_link_event_t ev;
    int byte_idx; /* index of the fed byte that produced the event */
    st_datagram_t dg;
} rec_t;

typedef struct {
    st_link_t l;
    uint32_t t; /* timestamp of the most recently fed byte */
    int fed;    /* bytes fed so far */
    rec_t rec[MAXEV];
    int nrec; /* non-NONE events recorded */
} ctx_t;

static void ctx_init(ctx_t *c, uint32_t t0)
{
    memset(c, 0, sizeof(*c));
    st_link_init(&c->l);
    c->t = t0;
}

/* Feed one byte, ST_CHAR_US + extra_gap after the previous one. */
static st_link_event_t feed1(ctx_t *c, uint8_t data, uint8_t flags, uint32_t extra_gap)
{
    st_rx_char_t ch;
    st_datagram_t out;
    st_link_event_t ev;
    c->t += ST_CHAR_US + extra_gap;
    ch.t_us = c->t;
    ch.data = data;
    ch.flags = flags;
    memset(&out, 0xEE, sizeof(out));
    ev = st_link_feed(&c->l, &ch, &out);
    if (ev != ST_LINK_NONE && c->nrec < MAXEV) {
        c->rec[c->nrec].ev = ev;
        c->rec[c->nrec].byte_idx = c->fed;
        c->rec[c->nrec].dg = out;
        c->nrec++;
    }
    c->fed++;
    return ev;
}

/* Feed a datagram: CMD flag on byte 0, spaced ST_CHAR_US. Returns the event of the last byte. */
static st_link_event_t feed_dg(ctx_t *c, const uint8_t *b, int n)
{
    st_link_event_t ev = ST_LINK_NONE;
    int i;
    for (i = 0; i < n; i++) {
        ev = feed1(c, b[i], i == 0 ? ST_F_CMD : 0, 0);
    }
    return ev;
}

static void test_depth_datagram(void)
{
    static const uint8_t d[] = {0x00, 0x02, 0x00, 0x25, 0x00};
    ctx_t c;
    int i;
    ctx_init(&c, 1000);
    for (i = 0; i < 4; i++) {
        CHECK_EQ_INT(feed1(&c, d[i], i == 0 ? ST_F_CMD : 0, 0), ST_LINK_NONE);
    }
    CHECK_EQ_INT(feed1(&c, d[4], 0, 0), ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.rec[0].byte_idx, 4);
    CHECK_EQ_INT(c.rec[0].dg.len, 5);
    CHECK_EQ_MEM(c.rec[0].dg.b, d, 5);
    CHECK_EQ_INT(c.rec[0].dg.flags, ST_F_CMD);
    CHECK_EQ_INT(c.l.st.datagrams, 1);
    CHECK_EQ_INT(c.l.st.foreign_bytes, 5);
    CHECK_EQ_INT(c.l.st.incomplete, 0);
    CHECK_EQ_INT(c.l.st.orphan, 0);
}

static void test_lamp_length_low_nibble(void)
{
    static const uint8_t d[] = {0x30, 0x70, 0x05};
    ctx_t c;
    ctx_init(&c, 1000);
    CHECK_EQ_INT(feed_dg(&c, d, 3), ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.rec[0].dg.len, 3);
    CHECK_EQ_MEM(c.rec[0].dg.b, d, 3);
    CHECK_EQ_INT(c.l.st.datagrams, 1);
}

static void test_max_length(void)
{
    uint8_t d[18];
    int i;
    ctx_t c;
    d[0] = 0xF0;
    d[1] = 0x0F;
    for (i = 2; i < 18; i++) d[i] = (uint8_t)(0x40 + i);
    ctx_init(&c, 1000);
    CHECK_EQ_INT(feed_dg(&c, d, 18), ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.rec[0].dg.len, 18);
    CHECK_EQ_MEM(c.rec[0].dg.b, d, 18);
    CHECK_EQ_INT(c.l.st.datagrams, 1);
}

static void test_new_cmd_mid_datagram(void)
{
    static const uint8_t partial[] = {0x00, 0x02, 0x00};
    static const uint8_t next[] = {0x30, 0x70, 0x05};
    ctx_t c;
    ctx_init(&c, 1000);
    CHECK_EQ_INT(feed_dg(&c, partial, 3), ST_LINK_NONE);
    /* new command byte arrives */
    CHECK_EQ_INT(feed1(&c, next[0], ST_F_CMD, 0), ST_LINK_INCOMPLETE);
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.rec[0].dg.len, 3);
    CHECK_EQ_MEM(c.rec[0].dg.b, partial, 3);
    CHECK_EQ_INT(c.l.st.incomplete, 1);
    CHECK_EQ_INT(c.l.st.datagrams, 0);
    /* the new datagram completes normally */
    CHECK_EQ_INT(feed1(&c, next[1], 0, 0), ST_LINK_NONE);
    CHECK_EQ_INT(feed1(&c, next[2], 0, 0), ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.nrec, 2);
    CHECK_EQ_INT(c.rec[1].dg.len, 3);
    CHECK_EQ_MEM(c.rec[1].dg.b, next, 3);
    CHECK_EQ_INT(c.l.st.datagrams, 1);
    CHECK_EQ_INT(c.l.st.incomplete, 1);
}

static void test_orphans(void)
{
    ctx_t c;
    ctx_init(&c, 1000);
    CHECK_EQ_INT(feed1(&c, 0x12, 0, 0), ST_LINK_NONE);
    CHECK_EQ_INT(feed1(&c, 0x34, 0, 0), ST_LINK_NONE);
    CHECK_EQ_INT(feed1(&c, 0x56, 0, 0), ST_LINK_NONE);
    CHECK_EQ_INT(c.nrec, 0);
    CHECK_EQ_INT(c.l.st.orphan, 3);
    CHECK_EQ_INT(c.l.st.datagrams, 0);
    CHECK_EQ_INT(c.l.st.incomplete, 0);
}

static void flagged_case(uint8_t bad_flag)
{
    static const uint8_t d[] = {0x00, 0x02, 0x00, 0x25, 0x00};
    ctx_t c;
    int i;
    ctx_init(&c, 1000);
    for (i = 0; i < 5; i++) {
        uint8_t f = (uint8_t)((i == 0 ? ST_F_CMD : 0) | (i == 2 ? bad_flag : 0));
        st_link_event_t ev = feed1(&c, d[i], f, 0);
        CHECK_EQ_INT(ev, i == 4 ? ST_LINK_FLAGGED : ST_LINK_NONE);
    }
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.rec[0].dg.len, 5);
    CHECK_EQ_INT(c.rec[0].dg.flags & bad_flag, bad_flag);
    CHECK_EQ_INT(c.l.st.flagged, 1);
    CHECK_EQ_INT(c.l.st.datagrams, 0);
    /* flagged datagrams do not count toward timing stats */
    CHECK_EQ_INT(c.l.st.min_idle_before_us, UINT32_MAX);
    /* a good datagram afterwards still works */
    CHECK_EQ_INT(feed_dg(&c, d, 5), ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.l.st.datagrams, 1);
}

static void test_flagged_ambiguous(void) { flagged_case(ST_F_AMBIGUOUS); }
static void test_flagged_frame_err(void) { flagged_case(ST_F_FRAME_ERR); }

static void test_flagged_cmd_byte(void)
{
    /* the first (command) byte itself ambiguous */
    static const uint8_t d[] = {0x30, 0x70, 0x05};
    ctx_t c;
    ctx_init(&c, 1000);
    feed1(&c, d[0], ST_F_CMD | ST_F_AMBIGUOUS, 0);
    feed1(&c, d[1], 0, 0);
    CHECK_EQ_INT(feed1(&c, d[2], 0, 0), ST_LINK_FLAGGED);
    CHECK_EQ_INT(c.l.st.flagged, 1);
    CHECK_EQ_INT(c.l.st.datagrams, 0);
}

static void test_echo_never_datagram(void)
{
    static const uint8_t d[] = {0x00, 0x02, 0x00, 0x25, 0x00};
    ctx_t c;
    int i;
    ctx_init(&c, 1000);
    for (i = 0; i < 5; i++) {
        uint8_t f = (uint8_t)(ST_F_ECHO | (i == 0 ? ST_F_CMD : 0));
        CHECK_EQ_INT(feed1(&c, d[i], f, 0), ST_LINK_NONE);
    }
    CHECK_EQ_INT(c.nrec, 0);
    CHECK_EQ_INT(c.l.st.echo_bytes, 5);
    CHECK_EQ_INT(c.l.st.foreign_bytes, 0);
    CHECK_EQ_INT(c.l.st.datagrams, 0);
    CHECK_EQ_INT(c.l.st.orphan, 0);
    CHECK_EQ_INT(c.l.st.incomplete, 0);
    /* ECHO_BAD echoes behave the same */
    CHECK_EQ_INT(feed1(&c, 0x00, (uint8_t)(ST_F_ECHO | ST_F_ECHO_BAD | ST_F_CMD), 0), ST_LINK_NONE);
    CHECK_EQ_INT(c.l.st.echo_bytes, 6);
}

static void test_echo_closes_foreign_datagram(void)
{
    static const uint8_t partial[] = {0x00, 0x02, 0x00};
    ctx_t c;
    ctx_init(&c, 1000);
    feed_dg(&c, partial, 3);
    CHECK_EQ_INT(feed1(&c, 0x84, ST_F_ECHO | ST_F_CMD, 0), ST_LINK_INCOMPLETE);
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.rec[0].dg.len, 3);
    CHECK_EQ_MEM(c.rec[0].dg.b, partial, 3);
    CHECK_EQ_INT(c.l.st.incomplete, 1);
    CHECK_EQ_INT(c.l.st.echo_bytes, 1);
    /* more echo bytes: nothing more to report */
    CHECK_EQ_INT(feed1(&c, 0x06, ST_F_ECHO, 0), ST_LINK_NONE);
    CHECK_EQ_INT(c.l.st.incomplete, 1);
    /* following foreign data bytes are orphans, nothing spliced */
    CHECK_EQ_INT(feed1(&c, 0x25, 0, 0), ST_LINK_NONE);
    CHECK_EQ_INT(feed1(&c, 0x00, 0, 0), ST_LINK_NONE);
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.l.st.orphan, 2);
    CHECK_EQ_INT(c.l.st.datagrams, 0);
}

static void test_idle_before_and_min(void)
{
    static const uint8_t d[] = {0x00, 0x02, 0x00, 0x25, 0x00};
    ctx_t c;
    st_rx_char_t ch;
    int i;

    /* first-ever datagram: no idle info */
    ctx_init(&c, 1000);
    CHECK_EQ_INT(feed_dg(&c, d, 5), ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.rec[0].dg.idle_before_us, 0);
    CHECK_EQ_INT(c.l.st.min_idle_before_us, 0);

    /* previous byte at t=1000, next cmd byte at 1000 + 30000 + ST_CHAR_US */
    ctx_init(&c, 0);
    ch.t_us = 1000;
    ch.data = 0x55;
    ch.flags = 0;
    st_link_feed(&c.l, &ch, NULL); /* orphan, only sets the bus time */
    c.t = 1000 + 30000;            /* feed1 adds ST_CHAR_US */
    feed1(&c, d[0], ST_F_CMD, 0);
    for (i = 1; i < 5; i++) feed1(&c, d[i], 0, 0);
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.rec[0].ev, ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.rec[0].dg.idle_before_us, 30000);
    CHECK_EQ_INT(c.l.st.min_idle_before_us, 30000);

    /* smaller idle lowers the min; larger does not raise it */
    feed1(&c, d[0], ST_F_CMD, 10000);
    for (i = 1; i < 5; i++) feed1(&c, d[i], 0, 0);
    CHECK_EQ_INT(c.rec[1].dg.idle_before_us, 10000);
    CHECK_EQ_INT(c.l.st.min_idle_before_us, 10000);
    feed1(&c, d[0], ST_F_CMD, 50000);
    for (i = 1; i < 5; i++) feed1(&c, d[i], 0, 0);
    CHECK_EQ_INT(c.rec[2].dg.idle_before_us, 50000);
    CHECK_EQ_INT(c.l.st.min_idle_before_us, 10000);

    /* flagged datagram with tiny idle does not affect the minimum */
    feed1(&c, d[0], ST_F_CMD, 0);
    feed1(&c, d[1], 0, 0);
    feed1(&c, d[2], ST_F_FRAME_ERR, 0);
    feed1(&c, d[3], 0, 0);
    CHECK_EQ_INT(feed1(&c, d[4], 0, 0), ST_LINK_FLAGGED);
    CHECK_EQ_INT(c.rec[3].dg.idle_before_us, 0);
    CHECK_EQ_INT(c.l.st.min_idle_before_us, 10000);

    st_link_reset_stats(&c.l);
    CHECK_EQ_INT(c.l.st.min_idle_before_us, UINT32_MAX);
    CHECK_EQ_INT(c.l.st.datagrams, 0);
    CHECK_EQ_INT(c.l.st.max_gap_us, 0);
}

static void test_max_gap(void)
{
    static const uint8_t d[] = {0x00, 0x02, 0x00, 0x25, 0x00};
    ctx_t c;
    ctx_init(&c, 1000);
    feed1(&c, d[0], ST_F_CMD, 0);
    feed1(&c, d[1], 0, 0);
    feed1(&c, d[2], 0, 5000); /* extra gap between byte 1 and 2 */
    feed1(&c, d[3], 0, 0);
    CHECK_EQ_INT(feed1(&c, d[4], 0, 0), ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.rec[0].dg.max_gap_us, 5000);
    CHECK_EQ_INT(c.l.st.max_gap_us, 5000);
    /* a tighter datagram later does not lower the stat */
    feed_dg(&c, d, 5);
    CHECK_EQ_INT(c.rec[1].dg.max_gap_us, 0);
    CHECK_EQ_INT(c.l.st.max_gap_us, 5000);
}

static void test_timestamp_wrap(void)
{
    static const uint8_t d[] = {0x00, 0x02, 0x00, 0x25, 0x00};
    ctx_t c;
    st_rx_char_t ch;
    ctx_init(&c, 0);
    ch.t_us = 0xFFFFFFFFu - 10000u;
    ch.data = 0x55;
    ch.flags = 0;
    st_link_feed(&c.l, &ch, NULL);
    /* cmd byte 30000 + ST_CHAR_US after the previous one: wraps past 0xFFFFFFFF */
    c.t = ch.t_us + 30000u;
    feed1(&c, d[0], ST_F_CMD, 0);
    CHECK(c.t < 100000u); /* really wrapped */
    feed1(&c, d[1], 0, 0);
    feed1(&c, d[2], 0, 7000);
    feed1(&c, d[3], 0, 0);
    CHECK_EQ_INT(feed1(&c, d[4], 0, 0), ST_LINK_DATAGRAM);
    CHECK_EQ_INT(c.rec[0].dg.idle_before_us, 30000);
    CHECK_EQ_INT(c.rec[0].dg.max_gap_us, 7000);

    /* a gap that itself straddles the wrap */
    ctx_init(&c, 0xFFFFFFFFu - 3 * ST_CHAR_US);
    feed1(&c, d[0], ST_F_CMD, 0);
    feed1(&c, d[1], 0, 0);
    feed1(&c, d[2], 0, 0);
    feed1(&c, d[3], 0, 4000);
    feed1(&c, d[4], 0, 0);
    CHECK_EQ_INT(c.nrec, 1);
    CHECK_EQ_INT(c.rec[0].dg.max_gap_us, 4000);
}

static void test_datagram_len(void)
{
    CHECK_EQ_INT(st_datagram_len(0x70), 3);
    CHECK_EQ_INT(st_datagram_len(0x0F), 18);
    CHECK_EQ_INT(st_datagram_len(0x02), 5);
    CHECK_EQ_INT(st_datagram_len(0x00), 3);
}

static void test_null_out(void)
{
    static const uint8_t d[] = {0x00, 0x02, 0x00, 0x25, 0x00};
    st_link_t l;
    st_rx_char_t ch;
    uint32_t t = 1000;
    int i;
    st_link_event_t ev = ST_LINK_NONE;

    st_link_init(&l);
    /* DATAGRAM */
    for (i = 0; i < 5; i++) {
        ch.t_us = t += ST_CHAR_US;
        ch.data = d[i];
        ch.flags = i == 0 ? ST_F_CMD : 0;
        ev = st_link_feed(&l, &ch, NULL);
    }
    CHECK_EQ_INT(ev, ST_LINK_DATAGRAM);
    /* FLAGGED */
    for (i = 0; i < 5; i++) {
        ch.t_us = t += ST_CHAR_US;
        ch.data = d[i];
        ch.flags = (uint8_t)((i == 0 ? ST_F_CMD : 0) | (i == 1 ? ST_F_AMBIGUOUS : 0));
        ev = st_link_feed(&l, &ch, NULL);
    }
    CHECK_EQ_INT(ev, ST_LINK_FLAGGED);
    /* INCOMPLETE via new cmd byte */
    for (i = 0; i < 2; i++) {
        ch.t_us = t += ST_CHAR_US;
        ch.data = d[i];
        ch.flags = i == 0 ? ST_F_CMD : 0;
        st_link_feed(&l, &ch, NULL);
    }
    ch.t_us = t += ST_CHAR_US;
    ch.data = 0x00;
    ch.flags = ST_F_CMD;
    CHECK_EQ_INT(st_link_feed(&l, &ch, NULL), ST_LINK_INCOMPLETE);
    /* INCOMPLETE via echo byte, then an orphan gives NONE */
    ch.t_us = t += ST_CHAR_US;
    ch.data = 0x02;
    ch.flags = 0;
    st_link_feed(&l, &ch, NULL);
    ch.t_us = t += ST_CHAR_US;
    ch.data = 0x00;
    ch.flags = ST_F_ECHO;
    CHECK_EQ_INT(st_link_feed(&l, &ch, NULL), ST_LINK_INCOMPLETE);
    ch.t_us = t += ST_CHAR_US;
    ch.data = 0x00;
    ch.flags = 0;
    CHECK_EQ_INT(st_link_feed(&l, &ch, NULL), ST_LINK_NONE);
    CHECK_EQ_INT(l.st.datagrams, 1);
    CHECK_EQ_INT(l.st.flagged, 1);
    CHECK_EQ_INT(l.st.incomplete, 2);
}

int main(void)
{
    RUN(test_depth_datagram);
    RUN(test_lamp_length_low_nibble);
    RUN(test_max_length);
    RUN(test_new_cmd_mid_datagram);
    RUN(test_orphans);
    RUN(test_flagged_ambiguous);
    RUN(test_flagged_frame_err);
    RUN(test_flagged_cmd_byte);
    RUN(test_echo_never_datagram);
    RUN(test_echo_closes_foreign_datagram);
    RUN(test_idle_before_and_min);
    RUN(test_max_gap);
    RUN(test_timestamp_wrap);
    RUN(test_datagram_len);
    RUN(test_null_out);
    return test_report();
}
