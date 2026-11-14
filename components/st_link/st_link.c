/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * SeaTalk 1 link layer: framing, echo suppression, timing diagnostics.
 * Logic from the st50-emulator's Assembler (src/main.cpp).
 */
#include "st_link.h"

#include <string.h>

void st_link_reset_stats(st_link_t *l)
{
    memset(&l->st, 0, sizeof(l->st));
    l->st.min_idle_before_us = UINT32_MAX;
}

void st_link_init(st_link_t *l)
{
    memset(l, 0, sizeof(*l));
    st_link_reset_stats(l);
}

static st_link_event_t abort_open(st_link_t *l, st_datagram_t *out)
{
    if (l->cur.len == 0) {
        return ST_LINK_NONE;
    }
    l->st.incomplete++;
    if (out) {
        *out = l->cur;
    }
    l->cur.len = 0;
    l->want = 0;
    return ST_LINK_INCOMPLETE;
}

st_link_event_t st_link_feed(st_link_t *l, const st_rx_char_t *c, st_datagram_t *out)
{
    const uint32_t prev_us = l->last_us;
    const bool have_prev = l->have_last;
    l->last_us = c->t_us;
    l->have_last = true;

    if (c->flags & ST_F_ECHO) {
        l->st.echo_bytes++;
        return abort_open(l, out);
    }
    l->st.foreign_bytes++;

    st_link_event_t ev = ST_LINK_NONE;
    if (c->flags & ST_F_CMD) {
        ev = abort_open(l, out);
        memset(&l->cur, 0, sizeof(l->cur));
        l->cur.t_us = c->t_us;
        const uint32_t start = c->t_us - ST_CHAR_US;
        l->cur.idle_before_us =
            (have_prev && (int32_t)(start - prev_us) > 0) ? (start - prev_us) : 0;
    } else if (l->cur.len == 0) {
        l->st.orphan++;
        return ST_LINK_NONE;
    } else {
        const uint32_t d = c->t_us - prev_us;
        if (d > ST_CHAR_US && d - ST_CHAR_US > l->cur.max_gap_us) {
            l->cur.max_gap_us = d - ST_CHAR_US;
        }
    }

    if (l->cur.len < ST_MAX_DATAGRAM) {
        l->cur.b[l->cur.len++] = c->data;
    }
    l->cur.flags |= c->flags;
    if (l->cur.len == 2) {
        l->want = st_datagram_len(l->cur.b[1]);
    }
    if (l->want == 0 || l->cur.len < l->want) {
        return ev; /* still collecting (ev may report an aborted predecessor) */
    }

    /* Complete. An INCOMPLETE report and a completion can't coincide: a
     * datagram is at least 3 bytes, so the command byte never completes one. */
    st_link_event_t done;
    if (l->cur.flags & (ST_F_AMBIGUOUS | ST_F_FRAME_ERR)) {
        l->st.flagged++;
        done = ST_LINK_FLAGGED;
    } else {
        l->st.datagrams++;
        if (l->cur.idle_before_us < l->st.min_idle_before_us) {
            l->st.min_idle_before_us = l->cur.idle_before_us;
        }
        if (l->cur.max_gap_us > l->st.max_gap_us) {
            l->st.max_gap_us = l->cur.max_gap_us;
        }
        done = ST_LINK_DATAGRAM;
    }
    if (out) {
        *out = l->cur;
    }
    l->cur.len = 0;
    l->want = 0;
    return done;
}
