/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux host-target stub for st_phy so espos_seatalk builds cleanly on the
 * ESP-IDF linux target without hardware UART registers.
 */
#include "st_phy.h"

#include <string.h>

static bool s_tx_enabled;
static st_phy_stats_t s_stats;
static st_phy_mismatch_t s_mismatch;

void st_phy_tx_hold_low(int tx_gpio)
{
    (void)tx_gpio;
}

esp_err_t st_phy_init(const st_phy_config_t *cfg)
{
    (void)cfg;
    s_tx_enabled = false;
    memset(&s_stats, 0, sizeof s_stats);
    memset(&s_mismatch, 0, sizeof s_mismatch);
    return ESP_OK;
}

esp_err_t st_phy_tx_enable(bool enable)
{
    s_tx_enabled = enable;
    return ESP_OK;
}

bool st_phy_tx_enabled(void)
{
    return s_tx_enabled;
}

st_tx_result_t st_phy_send(const uint8_t *bytes, uint8_t len, uint8_t *fail_idx)
{
    (void)bytes;
    (void)len;
    if (fail_idx) {
        *fail_idx = 0;
    }
    return ST_TX_NOT_READY;
}

uint32_t st_phy_quiet_us(void)
{
    return 0;
}

bool st_phy_line_low(void)
{
    return false;
}

bool st_phy_wait_quiet(uint32_t quiet_us, uint32_t timeout_ms)
{
    (void)quiet_us;
    (void)timeout_ms;
    return false;
}

bool st_phy_rx_pop(st_rx_char_t *out)
{
    (void)out;
    return false;
}

uint32_t st_phy_rx_pending(void)
{
    return 0;
}

void st_phy_set_rx_task(void *task_handle)
{
    (void)task_handle;
}

void st_phy_get_stats(st_phy_stats_t *out)
{
    if (out) {
        *out = s_stats;
    }
}

void st_phy_get_last_mismatch(st_phy_mismatch_t *out)
{
    if (out) {
        *out = s_mismatch;
    }
}

void st_phy_reset_stats(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    memset(&s_mismatch, 0, sizeof s_mismatch);
}

const char *st_tx_result_name(st_tx_result_t r)
{
    switch (r) {
    case ST_TX_OK: return "ok";
    case ST_TX_COLLISION: return "COLLISION";
    case ST_TX_NO_ECHO: return "NO-ECHO";
    case ST_TX_NOT_READY: return "NOT-READY";
    default: return "?";
    }
}
