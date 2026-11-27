/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal SeaTalk 1 <-> Signal K bridge example using espos_seatalk.
 */
#include <stdbool.h>

#include "esp_err.h"
#include "espos.h"
#include "espos_seatalk.h"
#include "sdkconfig.h"

void app_main(void)
{
#if CONFIG_EXAMPLE_SEATALK_INVERT
    const bool invert = true;
#else
    const bool invert = false;
#endif
    /* Release the SeaTalk bus first thing at boot: LOW for an inverting
     * interface, HIGH for a non-inverting one. */
    st_phy_tx_idle(CONFIG_EXAMPLE_SEATALK_TX_GPIO, invert);

    ESP_ERROR_CHECK(espos_start(NULL));

    st_core_config_t cfg = ST_CORE_CONFIG_DEFAULT();
    cfg.phy.uart_port = 1;
    cfg.phy.rx_gpio = CONFIG_EXAMPLE_SEATALK_RX_GPIO;
    cfg.phy.tx_gpio = CONFIG_EXAMPLE_SEATALK_TX_GPIO;
    cfg.phy.invert = invert;

    ESP_ERROR_CHECK(espos_seatalk_start(&cfg));
}
