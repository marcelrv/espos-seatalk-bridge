/*
 * SeaTalk bench-test application on top of st_core.
 *
 * Threading: st_core's st_task owns the bus (PHY, link, scheduler). This file
 * only uses the st_core API:
 *  - the RX callback runs on st_task: it prints (when the monitor is on) and
 *    bumps s_lamp, nothing else;
 *  - the UART0 console task only calls st_bench_key(), which enqueues the key;
 *  - the bench task takes the keys and does the heavy work (self-tests, stats).
 */
#include "st_bench.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "st_codec.h"
#include "st_con.h"
#include "st_core.h"
#include "st_link.h"
#include "st_sched.h"
#include "st_sk.h"
#include "st_sim.h"

static const char *TAG = "st_bench";

#define U(x) ((unsigned)(x))

#define SELFTEST_DATAGRAMS 16
#define SELFTEST_LEN 18
#define AUTO_STATS_MS 10000

static QueueHandle_t s_cmd_q;

/* Flags read by st_task (RX callback): single-word, written by the bench task. */
static volatile bool s_rx_monitor = true;
static volatile bool s_timing = false;
static volatile uint32_t s_lamp; /* 0x30 datagrams; counted on st_task */

/* Bench-task state. */
static bool s_auto_stats = true;
static bool s_continuous = false;

/* Idle wait presets (key w). */
static const uint32_t IDLE_PRESETS_US[] = {30000, 26000, 40000};
#define IDLE_JITTER_US 4000
static size_t s_idle_idx = 0;

/* Bus load snapshot. */
static int64_t s_load_t_us;
static uint32_t s_load_own;
static uint32_t s_load_foreign;

/* Cumulative continuous self-test. */
typedef struct {
    uint32_t rounds, ok, failed, bytes, echo_bad, ambiguous, frame_err;
} cont_t;
static cont_t s_cont;

void st_bench_print_help(void)
{
    st_con_printf("\n"
           "Keys:  h help | k self-test once | K continuous self-test on/off\n"
           "       v simulated boat on/off | V boat STATIC/SWEEP | p pause/resume periodic sending\n"
           "       1..5 all datagrams: off/to_sk/to_bus/both/auto | d datagram table\n"
           "       i idle-before diagnostics | w idle wait 30/26/40 ms | 8 heading via 89 <-> 9C\n"
           "       t stats | z reset stats | r RX monitor on/off | s Signal K stats\n"
           "       x timing per datagram on/off | a auto stats (10 s) on/off\n");
}

/* ---- RX callback (runs on st_task: short, no blocking) ---- */

static void on_rx(st_link_event_t ev, const st_datagram_t *dg, st_dec_status_t ds, const st_decoded_t *dec,
                  bool forward, void *arg)
{
    (void)arg;
    if (ev == ST_LINK_DATAGRAM && dg->b[0] == 0x30) {
        s_lamp++;
    }
    if (ev == ST_LINK_NONE || !s_rx_monitor) {
        return;
    }
    /* Build the whole line first: one st_con_printf call, so lines from
     * different tasks never interleave. */
    char line[256];
    size_t n = 0;
#define ADD(...)                                                                                                       \
    do {                                                                                                               \
        if (n < sizeof line) {                                                                                         \
            const int w_ = snprintf(line + n, sizeof line - n, __VA_ARGS__);                                           \
            if (w_ > 0) {                                                                                              \
                n += (size_t)w_;                                                                                       \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)
    for (uint8_t i = 0; i < dg->len; i++) {
        ADD("%02X ", dg->b[i]);
    }
    if (ev == ST_LINK_FLAGGED) {
        ADD("[FLAGGED]");
    } else if (ev == ST_LINK_INCOMPLETE) {
        ADD("(incomplete)");
    } else if (ev == ST_LINK_DATAGRAM) {
        if (ds == ST_DEC_OK && dec) {
            for (uint8_t i = 0; i < dec->n; i++) {
                ADD("-> %s %.4g ", st_quantity_name(dec->v[i].q), dec->v[i].value);
            }
            if (dec->flags) {
                ADD("(flags 0x%04X) ", U(dec->flags));
            }
        } else if (ds == ST_DEC_UNKNOWN) {
            ADD("-> unknown ");
        } else {
            ADD("-> bad length ");
        }
        if (forward) {
            ADD("[to SK]");
        }
    }
    if (s_timing) {
        ADD(" | idle-before %.2f ms", (double)dg->idle_before_us / 1000.0);
    }
#undef ADD
    st_con_printf("%s\n", line);
}

/* ---- statistics ---- */

static void print_stats(void)
{
    st_core_stats_t s;
    st_core_get_stats(&s);
    const int64_t now = esp_timer_get_time();

    st_con_printf("--------------------------------------------------------------\n");
    st_con_printf("TX   ok %u  retried-ok %u  collisions %u  no-echo %u  bus-busy %u  dropped %u | stale-skip %u  "
           "native-pause %u | idle-wait max %.1f ms\n",
           U(s.tx_ok), U(s.tx_retried_ok), U(s.tx_collisions), U(s.tx_no_echo), U(s.tx_bus_busy), U(s.tx_dropped),
           U(s.stale_skips), U(s.native_pauses), (double)s.idle_wait_max_us / 1000.0);
    st_con_printf("     state: paused %s  bus-held-low %s  no-echo-pause %s | sim %s %s | idle %.1f ms | up %u s\n",
           s.paused ? "yes" : "no", s.bus_held_low ? "yes" : "no", s.no_echo_pause ? "yes" : "no",
           st_sim_enabled() ? "on" : "off", st_sim_sweep() ? "SWEEP" : "STATIC",
           (double)IDLE_PRESETS_US[s_idle_idx] / 1000.0, U(now / 1000000));
    st_con_printf("PHY  rx bytes %u  echo ok %u bad %u  ambiguous %u  frame-err %u  overflow hw %u ring %u  "
           "max-bytes/ISR %u\n",
           U(s.phy.rx_bytes), U(s.phy.echo_ok), U(s.phy.echo_bad), U(s.phy.ambiguous), U(s.phy.frame_err),
           U(s.phy.hw_overflow), U(s.phy.ring_overflow), U(s.phy.max_bytes_per_isr));
    st_con_printf("LINK foreign datagrams %u  incomplete %u  orphan bytes %u  flagged %u  lamp(30) %u\n",
           U(s.link.datagrams), U(s.link.incomplete), U(s.link.orphan), U(s.link.flagged), U(s_lamp));

    if (s.own_bytes < s_load_own || s.foreign_bytes < s_load_foreign) {
        s_load_own = 0; /* counters were reset behind our back */
        s_load_foreign = 0;
    }
    const double el_us = (double)(now - s_load_t_us);
    if (el_us > 0) {
        const double own = (double)(s.own_bytes - s_load_own) * ST_CHAR_US * 100.0 / el_us;
        const double foreign = (double)(s.foreign_bytes - s_load_foreign) * ST_CHAR_US * 100.0 / el_us;
        st_con_printf("Bus load last %.1f s: %.0f%% (own %.0f%%, foreign %.0f%%)\n", el_us / 1e6, own + foreign, own,
               foreign);
    }
    s_load_t_us = now;
    s_load_own = s.own_bytes;
    s_load_foreign = s.foreign_bytes;

    st_con_printf("Heap free %u, min %u | con-drops %u\n", U(esp_get_free_heap_size()),
                  U(esp_get_minimum_free_heap_size()), U(st_con_drops()));
}

static void reset_stats(void)
{
    st_core_reset_stats();
    s_lamp = 0;
    memset(&s_cont, 0, sizeof s_cont);
    s_load_t_us = esp_timer_get_time();
    s_load_own = 0;
    s_load_foreign = 0;
    st_con_printf("Statistics reset\n");
}

/* ---- self-test ---- */

static void selftest_once(void)
{
    st_selftest_result_t r;
    const esp_err_t err = st_core_selftest(&r);
    if (err != ESP_OK) {
        st_con_printf("Self-test refused: %s\n", esp_err_to_name(err));
        return;
    }
    st_con_printf("Self-test done: %u ok, %u failed (collision %u, no-echo %u, bus-busy %u) | echo bad %u, ambiguous %u, "
           "frame-err %u -> %s\n",
           U(r.ok), U(SELFTEST_DATAGRAMS - r.ok), U(r.collision), U(r.no_echo), U(r.busy), U(r.echo_bad),
           U(r.ambiguous), U(r.frame_err), r.pass ? "PASS" : "FAIL");
}

static void cont_summary(const char *what)
{
    st_core_stats_t s;
    st_core_get_stats(&s);
    st_con_printf("Continuous self-test %s: rounds %u, datagrams ok %u failed %u, bytes sent %u | echo bad %u, ambiguous %u, "
           "frame-err %u | max-bytes/ISR %u\n",
           what, U(s_cont.rounds), U(s_cont.ok), U(s_cont.failed), U(s_cont.bytes), U(s_cont.echo_bad),
           U(s_cont.ambiguous), U(s_cont.frame_err), U(s.phy.max_bytes_per_isr));
}

/* One round; false when st_core refused (not running). */
static bool cont_round(void)
{
    st_selftest_result_t r;
    if (st_core_selftest(&r) != ESP_OK) {
        return false;
    }
    s_cont.rounds++;
    s_cont.ok += r.ok;
    s_cont.failed += SELFTEST_DATAGRAMS - r.ok;
    s_cont.bytes += r.ok * SELFTEST_LEN;
    s_cont.echo_bad += r.echo_bad;
    s_cont.ambiguous += r.ambiguous;
    s_cont.frame_err += r.frame_err;
    return true;
}

/* ---- datagram table ---- */

static void set_all_modes(st_mode_t mode)
{
    uint8_t n = 0;
    const st_dgram_info_t *t = st_codec_table(&n);
    unsigned refused = 0;
    for (uint8_t i = 0; i < n; i++) {
        st_dgram_cfg_t cfg;
        if (st_core_get_dgram_cfg(t[i].cmd, &cfg) != ESP_OK) {
            continue;
        }
        cfg.mode = mode; /* period and stale stay as they are */
        if (st_core_set_dgram_cfg(t[i].cmd, &cfg) != ESP_OK) {
            st_con_printf("  %02X %-24s refused mode %s\n", U(t[i].cmd), t[i].name, st_mode_name(mode));
            refused++;
        }
    }
    st_con_printf("All datagram types -> %s (%u of %u refused), planned rate %.1f datagrams/s\n", st_mode_name(mode),
           U(refused), U(n), st_core_planned_rate());
}

static void print_table(void)
{
    uint8_t n = 0;
    const st_dgram_info_t *t = st_codec_table(&n);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    st_con_printf("cmd name                     mode   period stale |   sent retried   coll   drop stale-skip native-pause "
           "rx-foreign\n");
    for (uint8_t i = 0; i < n; i++) {
        st_slot_t sl;
        if (st_core_get_slot(t[i].cmd, &sl) != ESP_OK) {
            continue;
        }
        const bool native = sl.cfg.mode == ST_MODE_AUTO && sl.native_seen_ms != INT64_MIN &&
                            (now_ms - sl.native_seen_ms) < ST_SCHED_NATIVE_HOLD_MS;
        st_con_printf(" %02X %-24s %-6s %6u %5u | %6u %7u %6u %6u %10u %12u %10u%s\n", U(t[i].cmd), t[i].name,
               st_mode_name(sl.cfg.mode), U(sl.cfg.period_ms), U(sl.cfg.stale_ms), U(sl.st.sent),
               U(sl.st.retried_ok), U(sl.st.collisions), U(sl.st.dropped), U(sl.st.stale_skips),
               U(sl.st.native_pauses), U(sl.st.rx_foreign), native ? "  native!" : "");
    }
    st_con_printf("planned rate %.1f datagrams/s\n", st_core_planned_rate());
}

static void print_idle_before(void)
{
    unsigned shown = 0;
    for (int cmd = 0; cmd < 256; cmd++) {
        uint32_t n, mn, mx;
        st_core_idle_before((uint8_t)cmd, &n, &mn, &mx);
        if (n > 0) {
            st_con_printf("cmd %02X  n %u  min %.2f ms  max %.2f ms\n", cmd, U(n), (double)mn / 1000.0, (double)mx / 1000.0);
            shown++;
        }
    }
    if (!shown) {
        st_con_printf("idle-before: no foreign datagrams yet\n");
    }
}

/* ---- commands ---- */

static void toggle(volatile bool *flag, const char *name)
{
    *flag = !*flag;
    st_con_printf("%s %s\n", name, *flag ? "ON" : "OFF");
}

static void print_sk_stats(void)
{
    st_sk_stats_t k;
    st_sk_get_stats(&k);
    st_con_printf("Signal K: own source %s | published %u, received %u, own-ignored %u, filtered %u, "
                  "mag derived %u, bad value %u\n",
                  st_sk_own_label(), U(k.published), U(k.received), U(k.own_ignored), U(k.filtered),
                  U(k.mag_derived), U(k.bad_value));
}

static void handle_cmd(char c)
{
    switch (c) {
    case 'h': st_bench_print_help(); break;
    case 'k':
        if (s_continuous) {
            st_con_printf("Stop the continuous self-test first (K)\n");
        } else {
            selftest_once();
        }
        break;
    case 'K':
        if (s_continuous) {
            s_continuous = false;
            cont_summary("stopped");
        } else {
            memset(&s_cont, 0, sizeof s_cont);
            s_continuous = true;
            st_con_printf("Continuous self-test started\n");
        }
        break;
    case 'v':
        st_sim_enable(!st_sim_enabled());
        st_con_printf("Simulated boat %s (%s)\n", st_sim_enabled() ? "ON" : "OFF (values go stale after 5 s)",
               st_sim_sweep() ? "SWEEP" : "STATIC");
        break;
    case 'V':
        st_sim_set_sweep(!st_sim_sweep());
        st_con_printf("Simulated boat mode: %s\n", st_sim_sweep() ? "SWEEP" : "STATIC");
        break;
    case 'p':
        st_core_pause(!st_core_paused());
        st_con_printf("Periodic sending %s\n", st_core_paused() ? "PAUSED" : "RESUMED");
        break;
    case '1': set_all_modes(ST_MODE_OFF); break;
    case '2': set_all_modes(ST_MODE_TO_SK); break;
    case '3': set_all_modes(ST_MODE_TO_BUS); break;
    case '4': set_all_modes(ST_MODE_BOTH); break;
    case '5': set_all_modes(ST_MODE_AUTO); break;
    case '8': {
        /* Heading via 89 (compass) instead of 9C, to check the odd-degree
         * flag on a display. Toggles: 89 at 1 s + 9C off <-> defaults. */
        st_dgram_cfg_t c89, c9c;
        st_core_get_dgram_cfg(0x89, &c89);
        st_core_get_dgram_cfg(0x9C, &c9c);
        const bool to89 = c89.period_ms == 0;
        c89.period_ms = to89 ? 1000 : 0;
        c9c.period_ms = to89 ? 0 : 1000;
        st_core_set_dgram_cfg(0x89, &c89);
        st_core_set_dgram_cfg(0x9C, &c9c);
        st_con_printf("Heading sent as %s\n", to89 ? "89 (compass)" : "9C (heading + rudder)");
        break;
    }
    case 'd': print_table(); break;
    case 'i': print_idle_before(); break;
    case 'w':
        s_idle_idx = (s_idle_idx + 1) % (sizeof IDLE_PRESETS_US / sizeof IDLE_PRESETS_US[0]);
        st_core_set_idle(IDLE_PRESETS_US[s_idle_idx], IDLE_JITTER_US);
        st_con_printf("Idle wait %.0f ms + 0..%.0f ms jitter\n", (double)IDLE_PRESETS_US[s_idle_idx] / 1000.0,
               (double)IDLE_JITTER_US / 1000.0);
        break;
    case 't': print_stats(); break;
    case 's': print_sk_stats(); break;
    case 'z': reset_stats(); break;
    case 'r': toggle(&s_rx_monitor, "RX monitor"); break;
    case 'x': toggle(&s_timing, "Datagram timing"); break;
    case 'a': {
        s_auto_stats = !s_auto_stats;
        st_con_printf("Auto statistics %s\n", s_auto_stats ? "ON" : "OFF");
        break;
    }
    default: break;
    }
}

/* ---- tasks ---- */

static void bench_task(void *arg)
{
    (void)arg;
    s_load_t_us = esp_timer_get_time();
    int64_t last_auto = esp_timer_get_time() / 1000;
    int64_t last_cont = last_auto;

    for (;;) {
        char c;
        if (xQueueReceive(s_cmd_q, &c, s_continuous ? 0 : pdMS_TO_TICKS(100)) == pdTRUE) {
            handle_cmd(c);
            continue; /* drain all pending keys first */
        }

        if (s_continuous && !cont_round()) {
            s_continuous = false;
            cont_summary("stopped (st_core not running)");
        }

        const int64_t t = esp_timer_get_time() / 1000;
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
    st_bench_print_help();
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
    st_core_add_rx(on_rx, NULL);
    st_sim_start();
    if (xTaskCreate(bench_task, "bench", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "bench task create failed");
    }
    if (xTaskCreate(console_task, "console", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
    }
}
