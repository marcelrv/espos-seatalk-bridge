/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * seatalk-bridge: SeaTalk 1 <-> Signal K bridge on espOS.
 *
 * Bench-test application on st_core: self-test, RX monitor, simulated boat,
 * statistics.
 */
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "espos.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_ST_BRIDGE_BENCH
#include "st_bench.h"
#endif
#include "st_con.h"
#include "st_core.h"
#include "st_phy.h"
#include "st_sk.h"

static const char *TAG = "seatalk-bridge";

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software reset";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
    }
}

static uint32_t bump_boot_count(void)
{
    nvs_handle_t h;
    uint32_t boots = 0;
    esp_err_t err = nvs_open("stbench", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return 0;
    }
    err = nvs_get_u32(h, "boots", &boots);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_get_u32 failed: %s", esp_err_to_name(err));
    }
    boots++;
    err = nvs_set_u32(h, "boots", boots);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot count store failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return boots;
}

void app_main(void)
{
    /* Very first thing, before espOS: release the bus (GPIO33 high jams it). */
    st_phy_tx_hold_low(33);

    esp_reset_reason_t rr = esp_reset_reason();

    ESP_ERROR_CHECK(espos_start(NULL));

    /* Console output mux right after espOS and before anything prints, so the
     * output mutex exists for all tasks. The telnet server (if compiled in)
     * starts on demand from the bench.telnet setting. */
    st_con_start();

    /* nvs_flash_init() is done by espOS. */
    uint32_t boots = bump_boot_count();
    st_con_printf("\nseatalk-bridge bench: reset reason %d (%s), boot #%u\n", (int)rr, reset_reason_name(rr),
                  (unsigned)boots);

    st_core_config_t cfg = ST_CORE_CONFIG_DEFAULT();
    esp_err_t err = st_core_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st_core_start failed: %s", esp_err_to_name(err));
    } else {
        err = st_sk_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "st_sk_start failed: %s", esp_err_to_name(err));
        }
    }

#if CONFIG_ST_BRIDGE_BENCH
    st_bench_start();
#endif
}
