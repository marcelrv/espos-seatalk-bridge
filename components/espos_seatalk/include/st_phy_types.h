/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * SeaTalk 1 PHY: plain data types, shared with st_link and host tests.
 * No ESP-IDF includes here, so this header compiles on a PC.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_MAX_DATAGRAM 18

/* Per-character flags (st_rx_char_t.flags). */
enum {
    ST_F_CMD = 0x01,       /* 9th (command) bit set: first byte of a datagram */
    ST_F_AMBIGUOUS = 0x02, /* parity flag could not be tied to this byte: CMD unsure */
    ST_F_FRAME_ERR = 0x04, /* framing error (stop bit low): collision or noise */
    ST_F_ECHO = 0x08,      /* our own transmission */
    ST_F_ECHO_BAD = 0x10,  /* echo did not match what we sent */
};

/* One received character. t_us is esp_timer time (low 32 bits) when the ISR
 * read it, which is about the middle of the stop bit. */
typedef struct {
    uint32_t t_us;
    uint8_t data;
    uint8_t flags;
} st_rx_char_t;

typedef enum {
    ST_TX_OK = 0,
    ST_TX_COLLISION, /* an echo byte differed: another talker */
    ST_TX_NO_ECHO,   /* a byte left, nothing came back: no 12 V or hardware fault */
    ST_TX_NOT_READY, /* TX not routed, bad length, or another send in progress */
} st_tx_result_t;

#ifdef __cplusplus
}
#endif
