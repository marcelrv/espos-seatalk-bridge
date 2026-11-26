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

void app_main(void)
{
    /* Release the SeaTalk bus first thing at boot (TX HIGH jams the bus). */
    st_phy_tx_hold_low(CONFIG_EXAMPLE_SEATALK_TX_GPIO);

    ESP_ERROR_CHECK(espos_start(NULL));

    st_core_config_t cfg = ST_CORE_CONFIG_DEFAULT();
    cfg.phy.uart_port = 1;
    cfg.phy.rx_gpio = CONFIG_EXAMPLE_SEATALK_RX_GPIO;
    cfg.phy.tx_gpio = CONFIG_EXAMPLE_SEATALK_TX_GPIO;
#if CONFIG_EXAMPLE_SEATALK_INVERT
    cfg.phy.invert = true;
#else
    cfg.phy.invert = false;
#endif

    ESP_ERROR_CHECK(espos_seatalk_start(&cfg));
}
