/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal SeaTalk 1 <-> Signal K bridge example using espos_seatalk.
 */
#include "esp_err.h"
#include "espos.h"
#include "espos_seatalk.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#define SEATALK_RX_GPIO 13
#define SEATALK_TX_GPIO 12
#else
#define SEATALK_RX_GPIO 32
#define SEATALK_TX_GPIO 33
#endif

void app_main(void)
{
    /* Release the SeaTalk bus first thing at boot (TX HIGH jams the bus). */
    st_phy_tx_hold_low(SEATALK_TX_GPIO);

    ESP_ERROR_CHECK(espos_start(NULL));

    st_core_config_t cfg = ST_CORE_CONFIG_DEFAULT();
    cfg.phy.uart_port = 1;
    cfg.phy.rx_gpio = SEATALK_RX_GPIO;
    cfg.phy.tx_gpio = SEATALK_TX_GPIO;
    cfg.phy.invert = true;

    ESP_ERROR_CHECK(espos_seatalk_start(&cfg));
}
