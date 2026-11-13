/*
 * SeaTalk 1 link layer (spec section 4): characters -> datagrams.
 *
 * Pure C, no hardware: feed it the characters st_phy delivers (or test
 * vectors on a PC) and it returns complete foreign datagrams.
 *  - A character with the command bit starts a datagram; length is
 *    (byte1 & 0x0F) + 3 (low nibble only: the ST50 sends 30 70 0X).
 *  - A new command byte before the datagram is complete -> INCOMPLETE.
 *  - A data byte with no datagram open -> orphan, dropped.
 *  - A datagram with an AMBIGUOUS or FRAME_ERR byte -> FLAGGED: counted and
 *    returned for diagnostics only; it must never reach the codec.
 *  - Echo bytes (our own transmission) never reach the parser. An echo byte
 *    also closes an open foreign datagram as incomplete, so nothing is ever
 *    spliced across our transmission.
 */
#pragma once

#include "st_phy_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_CHAR_US 2292u /* 11 bits at 4800 baud */

typedef struct {
    uint8_t len;
    uint8_t b[ST_MAX_DATAGRAM];
    uint8_t flags;           /* OR of the character flags */
    uint32_t t_us;           /* ISR timestamp of the first byte */
    uint32_t idle_before_us; /* quiet bus before this datagram started */
    uint32_t max_gap_us;     /* largest gap between two bytes beyond one char time */
} st_datagram_t;

typedef enum {
    ST_LINK_NONE = 0,
    ST_LINK_DATAGRAM,   /* valid foreign datagram in *out */
    ST_LINK_FLAGGED,    /* complete but contains a doubtful byte: diagnostics only */
    ST_LINK_INCOMPLETE, /* partial datagram in *out (aborted / cut off) */
} st_link_event_t;

typedef struct {
    uint32_t datagrams; /* valid foreign datagrams */
    uint32_t incomplete;
    uint32_t orphan;  /* data bytes without a command byte */
    uint32_t flagged; /* datagrams dropped for an ambiguous/frame-error byte */
    uint32_t echo_bytes;
    uint32_t foreign_bytes;
    uint32_t min_idle_before_us; /* over valid foreign datagrams; UINT32_MAX = none */
    uint32_t max_gap_us;         /* over valid foreign datagrams */
} st_link_stats_t;

typedef struct {
    st_datagram_t cur;
    uint8_t want;
    uint32_t last_us;  /* time of the previous character on the bus (any source) */
    bool have_last;
    st_link_stats_t st;
} st_link_t;

void st_link_init(st_link_t *l);
void st_link_reset_stats(st_link_t *l);

/* Feed one character. Returns an event; for DATAGRAM, FLAGGED and INCOMPLETE
 * the datagram is copied to *out. */
st_link_event_t st_link_feed(st_link_t *l, const st_rx_char_t *c, st_datagram_t *out);

/* Expected length from the attribute byte. */
static inline uint8_t st_datagram_len(uint8_t attr)
{
    return (uint8_t)((attr & 0x0F) + 3);
}

#ifdef __cplusplus
}
#endif
