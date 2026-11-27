/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * RX/TX lights enable pin. The config change callback only arms a one-shot
 * esp_timer; the timer reads seatalk_hw.leds and sets the pin (never reading
 * the config store from inside the config callback itself).
 */
#include "st_leds.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos_config.h"

static const char *TAG = "st_leds";

/* Board-owned namespace (moved out of the component's "seatalk" namespace;
 * espOS config migrations are intra-namespace, so an upgrade from pre-0.5
 * bench builds intentionally resets seatalk_hw.leds to its default of on). */
#define CFG_NS "seatalk_hw"
#define CFG_KEY "leds"

static gpio_num_t s_pin = GPIO_NUM_NC;
static bool s_active_high = true;
static esp_timer_handle_t s_timer;

static inline int level_for(bool on)
{
    return (on == s_active_high) ? 1 : 0;
}

static void apply_cb(void *arg)
{
    (void)arg;
    if (s_pin == GPIO_NUM_NC) {
        return;
    }
    bool on = true;
    (void)espos_config_get_bool(CFG_NS, CFG_KEY, &on);
    const int lvl = level_for(on);
    gpio_set_level(s_pin, lvl);
    /* Read the pad back: a level that differs means something on the board
     * holds the line. */
    const int pad = gpio_get_level(s_pin);
    ESP_LOGI(TAG, "RX/TX lights %s (GPIO%d set %s, reads %s)", on ? "on" : "off", (int)s_pin,
             lvl ? "high" : "low", pad ? "high" : "low");
}

static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    if (ns && key && strcmp(ns, CFG_NS) == 0 && strcmp(key, CFG_KEY) == 0 && s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_start_once(s_timer, 1000);
    }
}

void st_leds_start(int pin, bool active_high)
{
    if (pin < 0 || s_timer) {
        return;
    }
    s_pin = (gpio_num_t)pin;
    s_active_high = active_high;

    /* Lights on (the default) until the setting has been read. */
    gpio_set_level(s_pin, level_for(true));
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << (unsigned)pin,
        .mode = GPIO_MODE_INPUT_OUTPUT, /* input too, for the read-back */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d: gpio_config failed", pin);
        s_pin = GPIO_NUM_NC;
        return;
    }
    gpio_set_level(s_pin, level_for(true));
    const esp_timer_create_args_t ta = {.callback = apply_cb, .name = "st_leds"};
    if (esp_timer_create(&ta, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed");
        s_pin = GPIO_NUM_NC;
        return;
    }
    const esp_err_t err = espos_config_subscribe(on_config_change, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config subscribe failed: %s (seatalk_hw.leds only read at boot)",
                 esp_err_to_name(err));
    }
    esp_timer_start_once(s_timer, 0); /* initial state */
}
