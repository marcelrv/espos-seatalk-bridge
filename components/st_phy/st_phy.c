/*
 * SeaTalk 1 PHY, ported from st50-emulator/src/StUart.cpp (Arduino core 2.0.17,
 * IDF 4.4) to ESP-IDF 6. The ISR logic is unchanged; what differs:
 *  - blocking waits use a semaphore given by the ISR / an esp_timer, not
 *    vTaskDelay(1) polling (espOS runs FreeRTOS at 100 Hz: one tick = 10 ms);
 *  - quiet-bus measurement (st_phy_quiet_us) replaces the emulator's busy loop
 *    on the RX pin level;
 *  - the bit-bang echo window is gone (bit-bang was dropped, spec 5.3).
 */
#include "st_phy.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/uart_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/interrupts.h"
#include "soc/uart_struct.h"

static const char *TAG = "st_phy";

#define BAUD 4800
#define CHAR_US (11u * 1000000u / BAUD) /* 2291 us */
/* RX ring, power of two. 256 characters = 0.59 s of a completely full bus.
 * st_core drains it at least every ST_CORE_WAIT_SLICE_MS (50 ms) plus one
 * datagram send (<= 41 ms), so the worst case is ~40 characters. Was 1024
 * in the emulator; shrunk to save 6 KB of internal RAM (ring_overflow
 * counts it should that assumption ever fail). */
#define RING 256u
#define RX_ERR_MASK (UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR)

static uart_dev_t *s_hw;
static st_phy_config_t s_cfg;
static bool s_tx_routed;
static intr_handle_t s_intr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_tx_done;  /* given by the ISR when a datagram ends */
static SemaphoreHandle_t s_wait_sem; /* given by s_wait_timer */
static esp_timer_handle_t s_wait_timer;
static TaskHandle_t s_rx_task;

/* RX ring: single producer (ISR), single consumer task. */
static st_rx_char_t s_ring[RING];
static volatile uint32_t s_wr, s_rd;

/* Parity mode the UART is in right now (RX and TX share it). */
static volatile bool s_parity_odd;
/* esp_timer time of the last received byte, any source (guarded by s_mux). */
static int64_t s_last_rx_us;

enum { TX_IDLE = 0, TX_ACTIVE, TX_DONE };
static struct {
    uint8_t data[ST_MAX_DATAGRAM];
    uint8_t len;
    volatile uint8_t sent;   /* bytes handed to the UART */
    volatile uint8_t echoed; /* echo bytes received */
    volatile uint8_t state;
    volatile bool bad;
    volatile bool no_echo;
    volatile uint8_t fail_idx;
} s_tx;

static st_phy_stats_t s_st;
static st_phy_mismatch_t s_mm;

static inline bool IRAM_ATTR parity8(uint8_t b) /* 1 if an odd number of bits set */
{
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return b & 1;
}

/* Set the parity mode so the transmitted parity bit equals cmd.
 * EVEN parity bit = parity8(data); ODD parity bit = !parity8(data). */
static inline void IRAM_ATTR set_parity_for(uint8_t data, bool cmd)
{
    const bool odd = parity8(data) != cmd;
    uart_ll_set_parity(s_hw, odd ? UART_PARITY_ODD : UART_PARITY_EVEN);
    s_parity_odd = odd;
}

static inline void IRAM_ATTR ring_push(uint32_t t, uint8_t data, uint8_t flags)
{
    const uint32_t w = s_wr;
    if (w - s_rd >= RING) {
        s_st.ring_overflow++;
        return;
    }
    s_ring[w & (RING - 1)] = (st_rx_char_t){.t_us = t, .data = data, .flags = flags};
    __asm__ __volatile__("memw");
    s_wr = w + 1;
}

/* Under s_mux. Returns true when a waiter must be woken. */
static inline bool IRAM_ATTR tx_finish(void)
{
    s_tx.state = TX_DONE;
    uart_ll_disable_intr_mask(s_hw, UART_INTR_TX_DONE);
    uart_ll_set_parity(s_hw, UART_PARITY_EVEN);
    s_parity_odd = false;
    if (s_tx.bad) {
        s_st.tx_collision++;
    } else if (s_tx.no_echo) {
        s_st.tx_no_echo++;
    } else {
        s_st.tx_ok++;
    }
    return true;
}

static void IRAM_ATTR st_phy_isr(void *arg)
{
    (void)arg;
    bool wake_tx = false;
    bool got_rx = false;

    portENTER_CRITICAL_ISR(&s_mux);
    const uint32_t status = uart_ll_get_intsts_mask(s_hw);

    /* ---------------- RX ----------------
     * Loop until the FIFO stays empty: a byte that lands while we clear the
     * flags must not be left behind with its error flag wiped. */
    bool force_ambiguous = false;
    for (;;) {
        uint32_t n = uart_ll_get_rxfifo_len(s_hw);
        if (!n) {
            break;
        }
        /* Error flags come from the RAW register: they are not enabled as
         * interrupts, the ISR is driven by RXFIFO_FULL (threshold 1). */
        const uint32_t raw = s_hw->int_raw.val;
        const uint32_t n2 = uart_ll_get_rxfifo_len(s_hw);
        bool multi = force_ambiguous || n > 1;
        if (n2 != n) { /* a byte arrived while reading the flags */
            n = n2;
            multi = true;
        }
        const bool perr = raw & UART_INTR_PARITY_ERR;
        const bool ferr = raw & UART_INTR_FRAM_ERR;
        const int64_t now64 = esp_timer_get_time();
        const uint32_t now = (uint32_t)now64;
        if (n > s_st.max_bytes_per_isr) {
            s_st.max_bytes_per_isr = n;
        }

        for (uint32_t i = 0; i < n; i++) {
            uint8_t b;
            uart_ll_read_rxfifo(s_hw, &b, 1);

            /* One byte waiting: the flags belong to it. Several: a clear flag
             * is clear for all of them; a set flag is ambiguous. */
            const bool my_perr = multi ? false : perr;
            const bool ambiguous = multi && perr;
            const bool my_ferr = ferr; /* framing: flag the whole batch (rare) */

            const bool expected_parity = s_parity_odd ? !parity8(b) : parity8(b);
            const bool cmd = expected_parity ^ my_perr;

            uint8_t flags = 0;
            if (cmd) {
                flags |= ST_F_CMD;
            }
            if (ambiguous) {
                flags |= ST_F_AMBIGUOUS;
                s_st.ambiguous++;
            }
            if (my_ferr) {
                flags |= ST_F_FRAME_ERR;
                s_st.frame_err++;
            }
            if (my_perr) {
                s_st.parity_flag_seen++;
            }
            s_st.rx_bytes++;

            /* Echo check while a datagram of ours is on the bus. */
            if (s_tx.state == TX_ACTIVE && s_tx.echoed < s_tx.sent) {
                const uint8_t idx = s_tx.echoed;
                const bool want_cmd = (idx == 0);
                const bool ok = (b == s_tx.data[idx]) && (cmd == want_cmd) && !my_ferr && !ambiguous;
                flags |= ST_F_ECHO;
                if (ok) {
                    s_st.echo_ok++;
                } else {
                    flags |= ST_F_ECHO_BAD;
                    s_st.echo_bad++;
                    if (!s_tx.bad) {
                        s_tx.bad = true;
                        s_tx.fail_idx = idx;
                        s_mm.index = idx;
                        s_mm.expected = s_tx.data[idx];
                        s_mm.expected_cmd = want_cmd;
                        s_mm.got = b;
                        s_mm.got_flags = flags;
                    }
                }
                s_tx.echoed = idx + 1;
            }

            ring_push(now, b, flags);
            s_last_rx_us = now64;
            got_rx = true;
        }
        uart_ll_clr_intsts_mask(s_hw, UART_INTR_RXFIFO_FULL | RX_ERR_MASK);
        /* Anything in the FIFO now may have lost its flags in the clear above. */
        force_ambiguous = true;
    }
    if (status & UART_INTR_RXFIFO_OVF) {
        s_st.hw_overflow++;
        uart_ll_rxfifo_rst(s_hw);
        uart_ll_clr_intsts_mask(s_hw, UART_INTR_RXFIFO_OVF);
    }

    /* ---------------- TX ---------------- */
    if (status & UART_INTR_TX_DONE) {
        uart_ll_clr_intsts_mask(s_hw, UART_INTR_TX_DONE);
        if (s_tx.state == TX_ACTIVE) {
            if (s_tx.bad) {
                wake_tx = tx_finish(); /* collision: stop after this byte */
            } else if (s_tx.echoed < s_tx.sent) {
                s_tx.no_echo = true; /* byte left, nothing came back */
                s_tx.fail_idx = s_tx.echoed;
                wake_tx = tx_finish();
            } else if (s_tx.sent < s_tx.len) {
                const uint8_t idx = s_tx.sent;
                set_parity_for(s_tx.data[idx], idx == 0);
                uart_ll_write_txfifo(s_hw, &s_tx.data[idx], 1);
                s_tx.sent = idx + 1;
            } else {
                wake_tx = tx_finish(); /* all bytes sent and echoed */
            }
        }
    }
    TaskHandle_t rx_task = s_rx_task;
    portEXIT_CRITICAL_ISR(&s_mux);

    BaseType_t woken = pdFALSE;
    if (wake_tx) {
        xSemaphoreGiveFromISR(s_tx_done, &woken);
    }
    if (got_rx && rx_task) {
        vTaskNotifyGiveFromISR(rx_task, &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void wait_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreGive(s_wait_sem);
}

void st_phy_tx_hold_low(int tx_gpio)
{
    /* Output latch LOW before the driver is enabled: the pin never goes high. */
    gpio_set_level((gpio_num_t)tx_gpio, 0);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << tx_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level((gpio_num_t)tx_gpio, 0);
}

esp_err_t st_phy_init(const st_phy_config_t *cfg)
{
    if (s_hw) {
        return ESP_ERR_INVALID_STATE;
    }
    int rx_sig, tx_sig, src;
    switch (cfg->uart_port) {
    case 1:
        rx_sig = U1RXD_IN_IDX;
        tx_sig = U1TXD_OUT_IDX;
        src = ETS_UART1_INTR_SOURCE;
        break;
    case 2:
        rx_sig = U2RXD_IN_IDX;
        tx_sig = U2TXD_OUT_IDX;
        src = ETS_UART2_INTR_SOURCE;
        break;
    default:
        return ESP_ERR_INVALID_ARG; /* UART0 is the console */
    }
    (void)tx_sig;
    s_cfg = *cfg;

    /* TX first: make sure it is a GPIO held LOW (bus released). */
    st_phy_tx_hold_low(cfg->tx_gpio);

    s_tx_done = xSemaphoreCreateBinary();
    s_wait_sem = xSemaphoreCreateBinary();
    const esp_timer_create_args_t targs = {.callback = wait_timer_cb, .name = "st_phy_wait"};
    if (!s_tx_done || !s_wait_sem || esp_timer_create(&targs, &s_wait_timer) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t ucfg = {
        .baud_rate = BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(cfg->uart_port, &ucfg), TAG, "uart_param_config");
    /* Inversion BEFORE the TX pin is ever routed: an inverted TX idles LOW,
     * which leaves the bus released (brief section 3, rule 3). */
    ESP_RETURN_ON_ERROR(uart_set_line_inverse(cfg->uart_port,
                                              cfg->invert ? (UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV) : 0),
                        TAG, "uart_set_line_inverse");

    s_hw = UART_LL_GET_HW(cfg->uart_port);
    uart_ll_discard_error_data(s_hw, false); /* keep bytes with parity/framing "errors" */
    uart_ll_set_tx_idle_num(s_hw, 0);        /* no extra idle bits between bytes */
    uart_ll_set_rxfifo_full_thr(s_hw, 1);    /* interrupt on every byte */
    uart_ll_set_rx_tout(s_hw, 0);
    uart_ll_disable_intr_mask(s_hw, UART_LL_INTR_MASK);
    uart_ll_rxfifo_rst(s_hw);
    uart_ll_clr_intsts_mask(s_hw, UART_LL_INTR_MASK);
    s_parity_odd = false;

    /* RX pad: plain input, no pulls (external pull-up on the board). */
    const gpio_config_t rx = {
        .pin_bit_mask = 1ULL << cfg->rx_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rx), TAG, "rx gpio_config");
    esp_rom_gpio_connect_in_signal(cfg->rx_gpio, rx_sig, false);

    /* Between the inversion and the pin connection the UART briefly sees a
     * "line low" level and receives a phantom byte (measured: one byte with
     * the parity flag at every boot). Let any such character finish, then
     * flush it before the interrupt is enabled. */
    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    uart_ll_rxfifo_rst(s_hw);
    uart_ll_clr_intsts_mask(s_hw, UART_LL_INTR_MASK);

    s_last_rx_us = esp_timer_get_time();
    esp_err_t err = esp_intr_alloc(src, ESP_INTR_FLAG_IRAM, st_phy_isr, NULL, &s_intr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_intr_alloc: %s", esp_err_to_name(err));
        return err;
    }
    uart_ll_ena_intr_mask(s_hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_OVF);
    ESP_LOGI(TAG, "UART%d RX GPIO%d TX GPIO%d (held LOW), %s, ISR on core %d", cfg->uart_port,
             cfg->rx_gpio, cfg->tx_gpio, cfg->invert ? "inverted" : "not inverted",
             esp_intr_get_cpu(s_intr));
    return ESP_OK;
}

esp_err_t st_phy_tx_enable(bool enable)
{
    if (!s_hw) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == s_tx_routed) {
        return ESP_OK;
    }
    if (s_tx.state == TX_ACTIVE) {
        return ESP_ERR_INVALID_STATE;
    }
    const int tx_sig = (s_cfg.uart_port == 1) ? U1TXD_OUT_IDX : U2TXD_OUT_IDX;
    if (enable) {
        /* Inverted, idle UART TX -> output LOW -> bus released. */
        while (!uart_ll_is_tx_idle(s_hw)) {
        }
        esp_rom_gpio_connect_out_signal(s_cfg.tx_gpio, tx_sig, false, false);
    } else {
        gpio_set_level((gpio_num_t)s_cfg.tx_gpio, 0);
        esp_rom_gpio_connect_out_signal(s_cfg.tx_gpio, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_level((gpio_num_t)s_cfg.tx_gpio, 0);
    }
    s_tx_routed = enable;
    return ESP_OK;
}

bool st_phy_tx_enabled(void)
{
    return s_tx_routed;
}

st_tx_result_t st_phy_send(const uint8_t *bytes, uint8_t len, uint8_t *fail_idx)
{
    if (!s_hw || !s_tx_routed || len < 3 || len > ST_MAX_DATAGRAM) {
        return ST_TX_NOT_READY;
    }
    xSemaphoreTake(s_tx_done, 0); /* drop a stale give */
    portENTER_CRITICAL(&s_mux);
    if (s_tx.state == TX_ACTIVE) {
        portEXIT_CRITICAL(&s_mux);
        return ST_TX_NOT_READY;
    }
    memcpy(s_tx.data, bytes, len);
    s_tx.len = len;
    s_tx.echoed = 0;
    s_tx.bad = false;
    s_tx.no_echo = false;
    s_tx.fail_idx = 0;
    s_tx.state = TX_ACTIVE;
    set_parity_for(s_tx.data[0], true);
    uart_ll_clr_intsts_mask(s_hw, UART_INTR_TX_DONE);
    uart_ll_ena_intr_mask(s_hw, UART_INTR_TX_DONE);
    uart_ll_write_txfifo(s_hw, &s_tx.data[0], 1);
    s_tx.sent = 1;
    portEXIT_CRITICAL(&s_mux);

    /* Worst case len characters + slack; +1 tick for rounding at 100 Hz. */
    const TickType_t to = pdMS_TO_TICKS((len * CHAR_US) / 1000 + 20) + 1;
    if (xSemaphoreTake(s_tx_done, to) != pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        if (s_tx.state == TX_ACTIVE) {
            s_tx.no_echo = true;
            s_tx.fail_idx = s_tx.echoed;
            tx_finish();
        }
        portEXIT_CRITICAL(&s_mux);
    }

    portENTER_CRITICAL(&s_mux);
    const bool bad = s_tx.bad;
    const bool no_echo = s_tx.no_echo;
    if (fail_idx) {
        *fail_idx = s_tx.fail_idx;
    }
    s_tx.state = TX_IDLE;
    portEXIT_CRITICAL(&s_mux);

    if (bad) {
        return ST_TX_COLLISION;
    }
    return no_echo ? ST_TX_NO_ECHO : ST_TX_OK;
}

bool st_phy_line_low(void)
{
    const int level = gpio_get_level((gpio_num_t)s_cfg.rx_gpio);
    return s_cfg.invert ? (level != 0) : (level == 0);
}

uint32_t st_phy_quiet_us(void)
{
    if (!s_hw) {
        return 0;
    }
    /* A character in progress: RX state machine not idle, or line low now. */
    if (s_hw->status.st_urx_out != 0 || st_phy_line_low()) {
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    const int64_t last = s_last_rx_us;
    const bool txing = (s_tx.state == TX_ACTIVE);
    portEXIT_CRITICAL(&s_mux);
    if (txing) {
        return 0;
    }
    const int64_t q = esp_timer_get_time() - last;
    return q <= 0 ? 0 : (q > UINT32_MAX ? UINT32_MAX : (uint32_t)q);
}

bool st_phy_wait_quiet(uint32_t quiet_us, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        const uint32_t q = st_phy_quiet_us();
        if (q >= quiet_us) {
            return true;
        }
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        /* Sleep for the rest of the gap, then look again. If the bus was busy
         * (q == 0) this sleeps a full gap, which is never too long: the gap is
         * measured from the last byte, not from when we looked. */
        uint32_t sleep_us = quiet_us - q;
        if (sleep_us < 100) {
            sleep_us = 100;
        }
        xSemaphoreTake(s_wait_sem, 0);
        esp_timer_start_once(s_wait_timer, sleep_us);
        xSemaphoreTake(s_wait_sem, portMAX_DELAY);
    }
}

bool st_phy_rx_pop(st_rx_char_t *out)
{
    const uint32_t r = s_rd;
    if (r == s_wr) {
        return false;
    }
    *out = s_ring[r & (RING - 1)];
    __asm__ __volatile__("memw");
    s_rd = r + 1;
    return true;
}

uint32_t st_phy_rx_pending(void)
{
    return s_wr - s_rd;
}

void st_phy_set_rx_task(void *task_handle)
{
    portENTER_CRITICAL(&s_mux);
    s_rx_task = (TaskHandle_t)task_handle;
    portEXIT_CRITICAL(&s_mux);
}

void st_phy_get_stats(st_phy_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

void st_phy_get_last_mismatch(st_phy_mismatch_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_mm;
    portEXIT_CRITICAL(&s_mux);
}

void st_phy_reset_stats(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(&s_st, 0, sizeof(s_st));
    memset(&s_mm, 0, sizeof(s_mm));
    portEXIT_CRITICAL(&s_mux);
}

const char *st_tx_result_name(st_tx_result_t r)
{
    switch (r) {
    case ST_TX_OK:
        return "ok";
    case ST_TX_COLLISION:
        return "COLLISION";
    case ST_TX_NO_ECHO:
        return "NO-ECHO";
    case ST_TX_NOT_READY:
        return "NOT-READY";
    }
    return "?";
}
