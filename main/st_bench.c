/*
 * SeaTalk bench-test application.
 *
 * Threading: st_task owns st_link, all st_phy sending and every counter below.
 * The console task only calls st_bench_key(), which enqueues the key.
 */
#include "st_bench.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "st_link.h"
#include "st_phy.h"

static const char *TAG = "st_bench";

#define U(x) ((unsigned)(x))

#define SELFTEST_DATAGRAMS 16
#define SELFTEST_LEN 18
#define AUTO_STATS_MS 10000
#define BUS_LOW_LIMIT_MS 10000

static QueueHandle_t s_cmd_q;

/* ---- state owned by st_task ---- */
static st_link_t s_link;
static bool s_rx_monitor = true;
static bool s_timing = false;
static bool s_auto_stats = true;
static bool s_continuous = false;
static bool s_bus_held = false;
static uint32_t s_low_since_ms = 0; /* 0 = line not low */

static uint32_t s_lamp;
static uint32_t s_bus_busy;

/* load snapshot */
static int64_t s_load_t_us;
static uint32_t s_load_echo;
static uint32_t s_load_foreign;

/* cumulative continuous self-test */
typedef struct {
    uint32_t rounds, ok, failed, bytes, echo_bad, ambiguous, frame_err;
} cont_t;
static cont_t s_cont;

static void print_help(void)
{
    printf("\n"
           "Keys:  h help | k self-test once | K continuous self-test on/off\n"
           "       t stats | z reset stats | r RX monitor on/off\n"
           "       x timing per datagram on/off | a auto stats (10 s) on/off\n");
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- RX handling ---- */

static void print_datagram(const st_datagram_t *d, const char *tag)
{
    for (uint8_t i = 0; i < d->len; i++) {
        printf("%02X ", d->b[i]);
    }
    if (tag[0]) {
        printf("%s", tag);
    }
    if (s_timing) {
        printf(" | idle-before %.2f ms  max-gap +%.2f ms", (double)d->idle_before_us / 1000.0,
               (double)d->max_gap_us / 1000.0);
    }
    printf("\n");
}

static void handle_event(st_link_event_t ev, const st_datagram_t *d)
{
    switch (ev) {
    case ST_LINK_DATAGRAM:
        if (d->b[0] == 0x30) {
            s_lamp++;
        }
        if (s_rx_monitor) {
            print_datagram(d, "");
        }
        break;
    case ST_LINK_FLAGGED:
        if (s_rx_monitor) {
            print_datagram(d, "[FLAGGED]");
        }
        break;
    case ST_LINK_INCOMPLETE:
        if (s_rx_monitor) {
            print_datagram(d, "(incomplete)");
        }
        break;
    default:
        break;
    }
}

static void drain_rx(void)
{
    st_rx_char_t c;
    st_datagram_t d;
    while (st_phy_rx_pop(&c)) {
        st_link_event_t ev = st_link_feed(&s_link, &c, &d);
        if (ev != ST_LINK_NONE) {
            handle_event(ev, &d);
        }
    }
}

/* ---- bus held low (spec 3.2 rule 5) ---- */

static void check_bus_low(void)
{
    if (st_phy_line_low()) {
        uint32_t t = now_ms();
        if (s_low_since_ms == 0) {
            s_low_since_ms = t ? t : 1;
        } else if (!s_bus_held && (t - s_low_since_ms) > BUS_LOW_LIMIT_MS) {
            s_bus_held = true;
            ESP_LOGW(TAG, "bus held low for >%d s: sending disabled until it clears", BUS_LOW_LIMIT_MS / 1000);
        }
    } else {
        if (s_bus_held) {
            ESP_LOGW(TAG, "bus released again: sending re-enabled");
        }
        s_bus_held = false;
        s_low_since_ms = 0;
    }
}

/* ---- statistics ---- */

static void print_stats(void)
{
    st_phy_stats_t p;
    st_phy_get_stats(&p);
    const st_link_stats_t *l = &s_link.st;
    int64_t now = esp_timer_get_time();

    printf("--------------------------------------------------------------\n");
    printf("TX   ok %u  collisions %u  no-echo %u  bus-busy %u | tx routed %s | bus quiet %.1f ms | line low %s | up "
           "%u s\n",
           U(p.tx_ok), U(p.tx_collision), U(p.tx_no_echo), U(s_bus_busy), st_phy_tx_enabled() ? "yes" : "no",
           (double)st_phy_quiet_us() / 1000.0, st_phy_line_low() ? "yes" : "no", U(now / 1000000));
    printf("PHY  rx bytes %u  echo ok %u bad %u  ambiguous %u  frame-err %u  parity-flag %u  overflow hw %u ring %u  "
           "max-bytes/ISR %u\n",
           U(p.rx_bytes), U(p.echo_ok), U(p.echo_bad), U(p.ambiguous), U(p.frame_err), U(p.parity_flag_seen),
           U(p.hw_overflow), U(p.ring_overflow), U(p.max_bytes_per_isr));
    if (p.echo_bad > 0) {
        st_phy_mismatch_t m;
        st_phy_get_last_mismatch(&m);
        printf("     last echo mismatch: byte %u expected %02X%s got %02X%s%s%s\n", U(m.index), U(m.expected),
               m.expected_cmd ? "(cmd)" : "", U(m.got), (m.got_flags & ST_F_CMD) ? "(cmd)" : "",
               (m.got_flags & ST_F_AMBIGUOUS) ? " ambiguous" : "", (m.got_flags & ST_F_FRAME_ERR) ? " frame-err" : "");
    }
    printf("LINK foreign datagrams %u  incomplete %u  orphan bytes %u  flagged %u  lamp(30) %u\n", U(l->datagrams),
           U(l->incomplete), U(l->orphan), U(l->flagged), U(s_lamp));
    if (l->datagrams > 0) {
        printf("     foreign timing: idle-before min %.2f ms  max byte gap +%.2f ms\n",
               (double)l->min_idle_before_us / 1000.0, (double)l->max_gap_us / 1000.0);
    }

    double el_us = (double)(now - s_load_t_us);
    if (el_us > 0) {
        double own = (double)(l->echo_bytes - s_load_echo) * ST_CHAR_US * 100.0 / el_us;
        double foreign = (double)(l->foreign_bytes - s_load_foreign) * ST_CHAR_US * 100.0 / el_us;
        printf("Bus load last %.1f s: %.0f%% (own %.0f%%, foreign %.0f%%)\n", el_us / 1e6, own + foreign, own,
               foreign);
    }
    s_load_t_us = now;
    s_load_echo = l->echo_bytes;
    s_load_foreign = l->foreign_bytes;

    printf("Heap free %u, min %u\n", U(esp_get_free_heap_size()), U(esp_get_minimum_free_heap_size()));
}

static void reset_stats(void)
{
    st_phy_reset_stats();
    st_link_reset_stats(&s_link);
    s_lamp = 0;
    s_bus_busy = 0;
    memset(&s_cont, 0, sizeof s_cont);
    s_load_t_us = esp_timer_get_time();
    s_load_echo = 0;
    s_load_foreign = 0;
    printf("Statistics reset\n");
}

/* ---- self-test ---- */

typedef struct {
    uint32_t ok, collision, no_echo, busy, other;
    uint32_t echo_bad, ambiguous, frame_err;
} round_t;

static void selftest_round(round_t *r)
{
    st_phy_stats_t before, after;
    uint8_t buf[SELFTEST_LEN];

    memset(r, 0, sizeof *r);
    drain_rx();
    st_phy_get_stats(&before);

    for (int idx = 0; idx < SELFTEST_DATAGRAMS; idx++) {
        buf[0] = 0xF0;
        buf[1] = 0x0F;
        for (int i = 0; i < 16; i++) {
            buf[2 + i] = (uint8_t)(idx * 16 + i);
        }

        if (s_bus_held || !st_phy_wait_quiet(30000 + (esp_random() % 4000), 2000)) {
            r->busy++;
            s_bus_busy++;
        } else {
            uint8_t fail_idx = 0;
            st_tx_result_t res = st_phy_send(buf, SELFTEST_LEN, &fail_idx);
            switch (res) {
            case ST_TX_OK: r->ok++; break;
            case ST_TX_COLLISION: r->collision++; break;
            case ST_TX_NO_ECHO: r->no_echo++; break;
            default:
                r->other++;
                ESP_LOGE(TAG, "send returned %s", st_tx_result_name(res));
                break;
            }
        }
        drain_rx();
    }

    st_phy_get_stats(&after);
    r->echo_bad = after.echo_bad - before.echo_bad;
    r->ambiguous = after.ambiguous - before.ambiguous;
    r->frame_err = after.frame_err - before.frame_err;
}

static void print_last_mismatch(void)
{
    st_phy_mismatch_t m;
    st_phy_get_last_mismatch(&m);
    printf("Last echo mismatch: byte %u expected %02X%s got %02X%s%s%s\n", U(m.index), U(m.expected),
           m.expected_cmd ? "(cmd)" : "", U(m.got), (m.got_flags & ST_F_CMD) ? "(cmd)" : "",
           (m.got_flags & ST_F_AMBIGUOUS) ? " ambiguous" : "", (m.got_flags & ST_F_FRAME_ERR) ? " frame-err" : "");
}

static void selftest_once(void)
{
    if (s_bus_held) {
        printf("Self-test refused: bus held low\n");
        return;
    }
    if (!st_phy_tx_enabled()) {
        printf("Self-test refused: TX not routed (st_phy not ready)\n");
        return;
    }
    round_t r;
    selftest_round(&r);
    bool pass = r.ok == SELFTEST_DATAGRAMS && r.echo_bad == 0 && r.ambiguous == 0 && r.frame_err == 0;
    printf("Self-test done: %u ok, %u failed (collision %u, no-echo %u, bus-busy %u) | echo bad %u, ambiguous %u, "
           "frame-err %u -> %s\n",
           U(r.ok), U(SELFTEST_DATAGRAMS - r.ok), U(r.collision), U(r.no_echo), U(r.busy), U(r.echo_bad),
           U(r.ambiguous), U(r.frame_err), pass ? "PASS" : "FAIL");
    if (r.echo_bad > 0) {
        print_last_mismatch();
    }
}

static void cont_summary(const char *what)
{
    st_phy_stats_t p;
    st_phy_get_stats(&p);
    printf("Continuous self-test %s: rounds %u, datagrams ok %u failed %u, bytes sent %u | echo bad %u, ambiguous %u, "
           "frame-err %u | max-bytes/ISR %u\n",
           what, U(s_cont.rounds), U(s_cont.ok), U(s_cont.failed), U(s_cont.bytes), U(s_cont.echo_bad),
           U(s_cont.ambiguous), U(s_cont.frame_err), U(p.max_bytes_per_isr));
}

static void cont_round(void)
{
    round_t r;
    selftest_round(&r);
    s_cont.rounds++;
    s_cont.ok += r.ok;
    s_cont.failed += SELFTEST_DATAGRAMS - r.ok;
    s_cont.bytes += r.ok * SELFTEST_LEN;
    s_cont.echo_bad += r.echo_bad;
    s_cont.ambiguous += r.ambiguous;
    s_cont.frame_err += r.frame_err;
}

/* ---- commands ---- */

static void toggle(bool *flag, const char *name)
{
    *flag = !*flag;
    printf("%s %s\n", name, *flag ? "ON" : "OFF");
}

static void handle_cmd(char c)
{
    switch (c) {
    case 'h': print_help(); break;
    case 'k':
        if (s_continuous) {
            printf("Stop the continuous self-test first (K)\n");
        } else {
            selftest_once();
        }
        break;
    case 'K':
        if (s_continuous) {
            s_continuous = false;
            cont_summary("stopped");
        } else if (s_bus_held || !st_phy_tx_enabled()) {
            printf("Continuous self-test refused: bus held low or TX not routed\n");
        } else {
            memset(&s_cont, 0, sizeof s_cont);
            s_continuous = true;
            printf("Continuous self-test started\n");
        }
        break;
    case 't': print_stats(); break;
    case 'z': reset_stats(); break;
    case 'r': toggle(&s_rx_monitor, "RX monitor"); break;
    case 'x': toggle(&s_timing, "Datagram timing"); break;
    case 'a': toggle(&s_auto_stats, "Auto statistics"); break;
    default: break;
    }
}

/* ---- tasks ---- */

static void st_task(void *arg)
{
    (void)arg;
    st_phy_config_t cfg = ST_PHY_CONFIG_DEFAULT();

    /* The ISR is allocated on the calling core: init from this (core 1) task. */
    esp_err_t err = st_phy_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st_phy_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    st_phy_set_rx_task(xTaskGetCurrentTaskHandle());
    err = st_phy_tx_enable(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st_phy_tx_enable failed: %s (RX monitor only)", esp_err_to_name(err));
    }

    st_link_init(&s_link);
    s_load_t_us = esp_timer_get_time();
    uint32_t last_auto = now_ms();
    uint32_t last_cont = now_ms();
    printf("SeaTalk PHY up on core %d (UART%d, RX GPIO%d, TX GPIO%d)\n", xPortGetCoreID(), cfg.uart_port,
           cfg.rx_gpio, cfg.tx_gpio);

    for (;;) {
        bool run_cont = s_continuous && !s_bus_held;
        ulTaskNotifyTake(pdTRUE, run_cont ? 0 : pdMS_TO_TICKS(100));
        drain_rx();
        check_bus_low();

        char c;
        while (xQueueReceive(s_cmd_q, &c, 0) == pdTRUE) {
            handle_cmd(c);
        }

        if (s_continuous && !s_bus_held) {
            cont_round();
        } else if (s_continuous && s_bus_held) {
            s_continuous = false;
            cont_summary("stopped (bus held low)");
        }

        uint32_t t = now_ms();
        if (s_continuous && (t - last_cont) >= AUTO_STATS_MS) {
            last_cont = t;
            cont_summary("running");
        }
        if (s_auto_stats && (t - last_auto) >= AUTO_STATS_MS) {
            last_auto = t;
            print_stats();
        } else if (!s_auto_stats) {
            last_auto = t;
        }
    }
}

void st_bench_key(char c)
{
    if (c == '\r' || c == '\n' || s_cmd_q == NULL) {
        return;
    }
    if (xQueueSend(s_cmd_q, &c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, key '%c' dropped", c);
    }
}

/*
 * Console on UART0 (USB serial). A UART driver is installed only for RX
 * (uart_read_bytes). uart_vfs_dev_use_driver() is NOT called: stdout/printf and
 * ESP_LOG keep using the polling console TX path set up at boot, so logging is
 * unaffected, and we avoid stdin going through the driver.
 */
static void console_task(void *arg)
{
    (void)arg;
    if (!uart_is_driver_installed(UART_NUM_0)) {
        esp_err_t err = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install(UART0) failed: %s; console disabled", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
    }
    print_help();
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) == 1) {
            st_bench_key((char)c);
        }
    }
}

void st_bench_start(void)
{
    s_cmd_q = xQueueCreate(16, sizeof(char));
    if (s_cmd_q == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }
    if (xTaskCreatePinnedToCore(st_task, "st_task", 6144, NULL, 10, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "st_task create failed");
    }
    if (xTaskCreate(console_task, "console", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
    }
}
