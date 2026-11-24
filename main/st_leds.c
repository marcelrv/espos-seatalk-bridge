/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * RX/TX lights enable pin. The config change callback only arms a one-shot
 * esp_timer; the timer reads seatalk.leds and sets the pin (the same pattern
 * as st_con, so the callback never reads the config itself).
 */
#include "st_leds.h"

#include "sdkconfig.h"

#if CONFIG_ST_BRIDGE_LED_EN_GPIO >= 0

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos_config.h"

static const char *TAG = "st_leds";

#define PIN ((gpio_num_t)CONFIG_ST_BRIDGE_LED_EN_GPIO)
#define CFG_NS "seatalk"
#define CFG_KEY "leds"

#if CONFIG_ST_BRIDGE_LED_EN_ACTIVE_HIGH
#define LEVEL(on) ((on) ? 1 : 0)
#else
#define LEVEL(on) ((on) ? 0 : 1)
#endif

static esp_timer_handle_t s_timer;

static void apply_cb(void *arg)
{
    (void)arg;
    bool on = true;
    (void)espos_config_get_bool(CFG_NS, CFG_KEY, &on);
    gpio_set_level(PIN, LEVEL(on));
    ESP_LOGI(TAG, "RX/TX lights %s (GPIO%d %s)", on ? "on" : "off", (int)PIN, LEVEL(on) ? "high" : "low");
}

static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    if (ns && key && strcmp(ns, CFG_NS) == 0 && strcmp(key, CFG_KEY) == 0 && s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_start_once(s_timer, 1000);
    }
}

void st_leds_start(void)
{
    if (s_timer) {
        return;
    }
    /* Lights on (the default) until the setting has been read. */
    gpio_set_level(PIN, LEVEL(true));
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_ST_BRIDGE_LED_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d: gpio_config failed", (int)PIN);
        return;
    }
    gpio_set_level(PIN, LEVEL(true));
    const esp_timer_create_args_t ta = {.callback = apply_cb, .name = "st_leds"};
    if (esp_timer_create(&ta, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed");
        return;
    }
    const esp_err_t err = espos_config_subscribe(on_config_change, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config subscribe failed: %s (seatalk.leds only read at boot)", esp_err_to_name(err));
    }
    esp_timer_start_once(s_timer, 0); /* initial state */
}

#else /* no enable pin on this board */

void st_leds_start(void)
{
}

#endif
