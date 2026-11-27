/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos-seatalk-bridge: Raymarine SeaTalk 1 <-> Signal K bridge on espOS.
 *
 * Bus safety: TX HIGH jams the entire SeaTalk 1 bus. The chip's default TX
 * pin is driven LOW as the very first line of app_main(); if the user
 * overrode seatalk_hw.tx_pin in NVS, before_network() drives that validated
 * pin LOW right after espos_init() loads config and before Wi-Fi starts.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_system.h"
#include "soc/io_mux_reg.h"
#include "espos.h"
#include "espos_config.h"
#include "espos_seatalk.h"
#include "sdkconfig.h"
#include "st_api.h"
#include "st_leds.h"
#if CONFIG_ST_BRIDGE_BENCH
#include "nvs.h"
#include "st_bench.h"
#include "st_con.h"
#endif

static const char *TAG = "seatalk-bridge";

#define HW_NS "seatalk_hw"

typedef struct {
    int rx_pin;
    int tx_pin;
    bool invert;
    int led_en_pin;
    bool led_act_hi;
    bool bus_valid;
    bool default_tx_held;
} hw_cfg_t;

/* Pins the system reserved before app_main touched any pin (flash, PSRAM).
 * Taken as a snapshot because in ESP-IDF 6 gpio_config() of an output also
 * reserves the pin: after the TX hold-low, esp_gpio_is_reserved() would
 * report our own TX pin as taken. */
static uint64_t s_boot_reserved;

static void snapshot_reserved(void)
{
    for (int pin = 0; pin < 64; pin++) {
        if (GPIO_IS_VALID_GPIO(pin) && esp_gpio_is_reserved(BIT64(pin))) {
            s_boot_reserved |= BIT64(pin);
        }
    }
}

/* Strapping pins, sampled at reset for boot mode, flash voltage (ESP32
 * GPIO12) and boot log: a SeaTalk interface or a lights driver on one can
 * stop the chip from booting. IDF has no capability macro for them, so the
 * datasheet lists. */
#if CONFIG_IDF_TARGET_ESP32
#define ST_STRAPPING_PINS (BIT64(0) | BIT64(2) | BIT64(5) | BIT64(12) | BIT64(15))
#elif CONFIG_IDF_TARGET_ESP32S3
#define ST_STRAPPING_PINS (BIT64(0) | BIT64(3) | BIT64(45) | BIT64(46))
#else
#define ST_STRAPPING_PINS 0ULL
#endif

/* The internal USB PHY's D-/D+ (USB Serial/JTAG: console, flashing). */
#if defined(USB_INT_PHY0_DM_GPIO_NUM) && defined(USB_INT_PHY0_DP_GPIO_NUM)
#define ST_USB_PINS (BIT64(USB_INT_PHY0_DM_GPIO_NUM) | BIT64(USB_INT_PHY0_DP_GPIO_NUM))
#else
#define ST_USB_PINS 0ULL
#endif

/* A pin from the config is refused (-1) when the chip cannot use it, when
 * flash or PSRAM hold it, or when it is a strapping or USB pin. The build's
 * own default (build_default) skips the last check: the board it was built
 * for is wired that way on purpose. */
static int validate_gpio(const char *key, int pin, bool output, int build_default)
{
    if (pin < 0) {
        return -1;
    }
    const bool valid = output ? GPIO_IS_VALID_OUTPUT_GPIO(pin) : GPIO_IS_VALID_GPIO(pin);
    if (!valid) {
        ESP_LOGE(TAG, "%s.%s: GPIO %d cannot be used as %s on this chip", HW_NS, key, pin,
                 output ? "an output" : "an input");
        return -1;
    }
    if (s_boot_reserved & BIT64(pin)) {
        ESP_LOGE(TAG, "%s.%s: GPIO %d is reserved by flash or PSRAM", HW_NS, key, pin);
        return -1;
    }
    if (pin != build_default) {
        if (ST_STRAPPING_PINS & BIT64(pin)) {
            ESP_LOGE(TAG, "%s.%s: GPIO %d is a strapping pin; wiring on it can stop the chip booting",
                     HW_NS, key, pin);
            return -1;
        }
        if (ST_USB_PINS & BIT64(pin)) {
            ESP_LOGE(TAG, "%s.%s: GPIO %d is a USB data pin (USB Serial/JTAG)", HW_NS, key, pin);
            return -1;
        }
    }
    return pin;
}

/* Stop driving the build's default TX pin when it is no longer TX, leaving
 * only a weak pull-down. Always a pull-down, whatever the new invert setting:
 * the default pin is wired as on this build's own board, whose TX interface
 * inverts, so a pull-up there would switch its transistor on and hold the
 * bus if the board is in fact still wired that way. */
static void tx_release_default(int pin)
{
    /* The early hold reserved the pin as an output; hand it back first, or
     * gpio_config() logs "conflict found" for it. */
    esp_gpio_revoke(BIT64(pin));
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << (unsigned)pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
}

static esp_err_t before_network_cb(void *arg)
{
    hw_cfg_t *hw = (hw_cfg_t *)arg;

    int32_t rx = -1;
    if (espos_config_get_i32(HW_NS, "rx_pin", &rx) != ESP_OK || rx < 0) {
        rx = CONFIG_ST_BRIDGE_RX_GPIO;
    }
    int32_t tx = -1;
    if (espos_config_get_i32(HW_NS, "tx_pin", &tx) != ESP_OK || tx < 0) {
        tx = CONFIG_ST_BRIDGE_TX_GPIO;
    }
    bool invert = true;
    (void)espos_config_get_bool(HW_NS, "invert", &invert);

    /* The descriptor's default is this build's pin; -1 = no lights pin. */
    int32_t led_en = CONFIG_ST_BRIDGE_LED_EN_GPIO;
    if (espos_config_get_i32(HW_NS, "led_en_pin", &led_en) != ESP_OK) {
        led_en = CONFIG_ST_BRIDGE_LED_EN_GPIO;
    }
    bool led_act_hi = true;
    (void)espos_config_get_bool(HW_NS, "led_act_hi", &led_act_hi);

    hw->rx_pin = validate_gpio("rx_pin", (int)rx, false, CONFIG_ST_BRIDGE_RX_GPIO);
    hw->tx_pin = validate_gpio("tx_pin", (int)tx, true, CONFIG_ST_BRIDGE_TX_GPIO);
    hw->invert = invert;
    hw->led_act_hi = led_act_hi;

    hw->bus_valid = (hw->rx_pin >= 0 && hw->tx_pin >= 0 && hw->rx_pin != hw->tx_pin);
    if (hw->rx_pin >= 0 && hw->tx_pin >= 0 && hw->rx_pin == hw->tx_pin) {
        ESP_LOGE(TAG, "%s: RX and TX cannot both be GPIO %d", HW_NS, hw->tx_pin);
    }

    if (hw->rx_pin != CONFIG_ST_BRIDGE_RX_GPIO || hw->tx_pin != CONFIG_ST_BRIDGE_TX_GPIO) {
        ESP_LOGW(TAG, "%s: pins RX %d / TX %d differ from this build's RX %d / TX %d", HW_NS,
                 hw->rx_pin, hw->tx_pin, CONFIG_ST_BRIDGE_RX_GPIO, CONFIG_ST_BRIDGE_TX_GPIO);
    }

    /* Put the configured TX pin in its idle (bus released) state before
     * Wi-Fi starts. A default pin that app_main held LOW but that is no longer
     * the TX pin keeps a weak pull-down (see tx_release_default), never
     * gpio_reset_pin()'s pull-up: if the setting is a typo and the real TX
     * transistor is still on the default pin, a pull-up there would switch it
     * on and hold the whole bus. */
    if (hw->tx_pin >= 0) {
        st_phy_tx_idle(hw->tx_pin, hw->invert);
        if (hw->default_tx_held && (hw->tx_pin != CONFIG_ST_BRIDGE_TX_GPIO || !hw->invert)) {
            if (hw->tx_pin != CONFIG_ST_BRIDGE_TX_GPIO) {
                tx_release_default(CONFIG_ST_BRIDGE_TX_GPIO);
            }
            hw->default_tx_held = false;
        }
    }

    hw->led_en_pin = (led_en >= 0) ? validate_gpio("led_en_pin", (int)led_en, true, CONFIG_ST_BRIDGE_LED_EN_GPIO) : -1;
    if (hw->led_en_pin >= 0 && (hw->led_en_pin == hw->tx_pin || hw->led_en_pin == hw->rx_pin)) {
        ESP_LOGE(TAG, "%s.led_en_pin: GPIO %d conflicts with SeaTalk RX/TX", HW_NS, hw->led_en_pin);
        hw->led_en_pin = -1;
    }

    return ESP_OK;
}

#if CONFIG_ST_BRIDGE_BENCH
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
#endif

void app_main(void)
{
    snapshot_reserved();
    bool default_tx_held = false;
    /* Very first thing, before espOS: release the bus on the default TX pin
     * (TX HIGH jams the SeaTalk 1 bus). */
    if (GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_ST_BRIDGE_TX_GPIO) &&
        !(s_boot_reserved & BIT64(CONFIG_ST_BRIDGE_TX_GPIO))) {
        st_phy_tx_hold_low(CONFIG_ST_BRIDGE_TX_GPIO);
        default_tx_held = true;
    }

    hw_cfg_t hw = {
        .rx_pin = -1,
        .tx_pin = -1,
        .invert = true,
        .led_en_pin = -1,
        .led_act_hi = true,
        .bus_valid = false,
        .default_tx_held = default_tx_held,
    };

    espos_start_opts_t opts = ESPOS_START_OPTS_DEFAULT;
    opts.before_network = before_network_cb;
    opts.arg = &hw;
    ESP_ERROR_CHECK(espos_start(&opts));

    st_leds_start(hw.led_en_pin, hw.led_act_hi);

#if CONFIG_ST_BRIDGE_BENCH
    esp_reset_reason_t rr = esp_reset_reason();
    st_con_start();
    uint32_t boots = bump_boot_count();
    st_con_printf("\nseatalk-bridge bench: reset reason %d (%s), boot #%u\n", (int)rr,
                  reset_reason_name(rr), (unsigned)boots);
#endif

    bool bus_running = false;
    if (!hw.bus_valid) {
        ESP_LOGE(TAG, "SeaTalk bus stopped: fix %s.rx_pin / tx_pin in the web UI and restart", HW_NS);
    } else {
        st_core_config_t cfg = ST_CORE_CONFIG_DEFAULT();
        cfg.phy.uart_port = CONFIG_ST_BRIDGE_UART_PORT;
        cfg.phy.rx_gpio = hw.rx_pin;
        cfg.phy.tx_gpio = hw.tx_pin;
        cfg.phy.invert = hw.invert;

        esp_err_t err = espos_seatalk_start(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "espos_seatalk_start failed: %s", esp_err_to_name(err));
        } else {
            bus_running = true;
            ESP_LOGI(TAG, "SeaTalk bus live on UART%d (RX=%d TX=%d invert=%s); status: GET /api/v1/seatalk",
                     CONFIG_ST_BRIDGE_UART_PORT, hw.rx_pin, hw.tx_pin, hw.invert ? "on" : "off");
        }
    }

    const esp_err_t api_err = st_api_register(bus_running, hw.tx_pin, hw.rx_pin, hw.invert);
    if (api_err != ESP_OK) {
        ESP_LOGW(TAG, "GET /api/v1/seatalk not registered: %s", esp_err_to_name(api_err));
    }

#if CONFIG_ST_BRIDGE_BENCH
    /* The bench and simulator call into st_core, whose lock only exists once
     * the bus has started. */
    if (bus_running) {
        st_bench_start();
    } else {
        st_con_printf("bench console off: SeaTalk bus not running (check %s pins)\n", HW_NS);
    }
#endif
}
