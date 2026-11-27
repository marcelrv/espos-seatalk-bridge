/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * SeaTalk 1 PHY on the ESP32 / ESP32-S3 hardware UART (spec section 3).
 *
 * 9th (command) bit: the UART runs 8 data bits + parity; the parity bit sits
 * exactly where SeaTalk's command bit is.
 *   RX: cmd = expectedParity(data, current mode) XOR parity-error flag. The
 *       UART FIFO keeps no per-byte error flags, so the ISR runs for
 *       every byte (FIFO threshold 1). Several bytes waiting with a parity
 *       flag set -> all marked AMBIGUOUS.
 *   TX: per byte, EVEN or ODD parity so the parity bit equals the command
 *       bit. Bytes go out one at a time from the TX-done interrupt, and every
 *       byte's echo is checked before the next one is written.
 *
 * Bus safety (spec 3.2): call st_phy_tx_hold_low() as the very first thing in
 * app_main. st_phy_init() sets the UART inversion before TX is ever routed;
 * st_phy_tx_enable(true) routes it. An inverted, idle UART TX drives LOW,
 * which leaves the bus released, also after a panic or a debugger halt (at
 * most the one byte in the FIFO still goes out).
 */
#pragma once

#include "esp_err.h"
#include "st_phy_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_port; /* UART_NUM_1 */
    int rx_gpio;   /* 32 on the board; the app sets it from Kconfig */
    int tx_gpio;   /* 33 on the board; the app sets it from Kconfig */
    bool invert;   /* true on the board: both lines inverted */
} st_phy_config_t;

#define ST_PHY_CONFIG_DEFAULT() \
    { .uart_port = 1, .rx_gpio = 32, .tx_gpio = 33, .invert = true }

typedef struct {
    uint32_t rx_bytes;
    uint32_t parity_flag_seen;  /* bytes with the parity flag set (normal for some cmd bits) */
    uint32_t ambiguous;
    uint32_t frame_err;
    uint32_t hw_overflow;       /* UART FIFO overflow */
    uint32_t ring_overflow;     /* our ring full: consumer too slow */
    uint32_t echo_ok;
    uint32_t echo_bad;
    uint32_t max_bytes_per_isr; /* >1: the ISR was late at least once */
    uint32_t tx_ok;
    uint32_t tx_collision;
    uint32_t tx_no_echo;
} st_phy_stats_t;

/* Details of the last echo mismatch. */
typedef struct {
    uint8_t index; /* byte index in the datagram */
    uint8_t expected;
    uint8_t got;
    uint8_t got_flags;
    bool expected_cmd;
} st_phy_mismatch_t;

/* Drive the TX pin as GPIO output LOW (bus released through an inverting
 * interface). Call first in app_main. Same as st_phy_tx_idle(tx_gpio, true). */
void st_phy_tx_hold_low(int tx_gpio);

/* Drive the TX pin as GPIO output at its idle level, which leaves the bus
 * released: LOW when the interface inverts, HIGH when it does not. */
void st_phy_tx_idle(int tx_gpio, bool invert);

/* Configure the UART (4800 baud, 8 data + parity, inversion), connect RX and
 * install the IRAM ISR on the calling core. TX stays a GPIO at its idle level
 * (st_phy_tx_idle with cfg->invert). */
esp_err_t st_phy_init(const st_phy_config_t *cfg);

/* Route TX to the UART (true) or back to a GPIO at its idle level (false).
 * Not while sending. */
esp_err_t st_phy_tx_enable(bool enable);
bool st_phy_tx_enabled(void);

/* Send one datagram, byte by byte with an echo check per byte. Blocks the
 * calling task (no busy wait) until done. The caller is responsible for the
 * idle wait (st_phy_wait_quiet()). fail_idx (optional) gets the failing byte. */
st_tx_result_t st_phy_send(const uint8_t *bytes, uint8_t len, uint8_t *fail_idx);

/* Microseconds the bus has been quiet: 0 while a character is being received
 * or the line is held low. Measured from the last received byte (about mid
 * stop bit), so it errs on the polite side. */
uint32_t st_phy_quiet_us(void);

/* True while the line is at the "bus low" level right now. */
bool st_phy_line_low(void);

/* Block until the bus has been quiet for quiet_us. Returns false after
 * timeout_ms without such a gap. Sleeps on an esp_timer, no busy wait. */
bool st_phy_wait_quiet(uint32_t quiet_us, uint32_t timeout_ms);

/* Pop the next received character (echo included, flagged). */
bool st_phy_rx_pop(st_rx_char_t *out);

/* Characters waiting in the ring. */
uint32_t st_phy_rx_pending(void);

/* The ISR sends a task notification (xTaskNotifyGive) to this task for every
 * received character. NULL to stop. The handle is a FreeRTOS TaskHandle_t. */
void st_phy_set_rx_task(void *task_handle);

void st_phy_get_stats(st_phy_stats_t *out);
void st_phy_get_last_mismatch(st_phy_mismatch_t *out);
void st_phy_reset_stats(void);

const char *st_tx_result_name(st_tx_result_t r);

#ifdef __cplusplus
}
#endif
