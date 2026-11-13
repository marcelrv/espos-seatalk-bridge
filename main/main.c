/*
 * seatalk-bridge: SeaTalk 1 <-> Signal K bridge on espOS.
 *
 * Bench-test application: PHY/link self-test, RX monitor, statistics.
 */
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "espos.h"
#include "nvs.h"
#include "st_bench.h"
#include "st_phy.h"

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

    /* nvs_flash_init() is done by espOS. */
    uint32_t boots = bump_boot_count();
    printf("\nseatalk-bridge bench: reset reason %d (%s), boot #%u\n", (int)rr, reset_reason_name(rr),
           (unsigned)boots);

    st_bench_start();
}
