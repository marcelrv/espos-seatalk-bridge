/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * Output mux of the bench console (UART0, optionally mirrored to telnet,
 * TCP port 23).
 *
 * SECURITY: the telnet console is unauthenticated and unencrypted. Anyone on
 * the LAN can send keys (start/stop the simulator, flood the bus). Bench use
 * only. It is compiled in only with CONFIG_ST_BRIDGE_TELNET and is switched
 * on at run time by the config setting bench.telnet (default off); build
 * without CONFIG_ST_BRIDGE_TELNET for anything else.
 *
 * On demand: while the setting is off nothing is allocated and no task or
 * socket exists. A config change arms a one-shot esp_timer (debounce, no
 * permanent task); its callback reads the setting and, when it turned on,
 * allocates the line buffers and creates the server task. When it turned
 * off, the callback only clears s_want: the server task notices within
 * 200 ms, closes its sockets, frees the buffers under s_mtx and deletes
 * itself, then re-arms the timer so a quick off->on is picked up.
 *
 * Threading: one server task owns accept/recv and the lifetime of the client
 * socket. Any task may call st_con_printf(); output (format buffer, socket
 * send, UART write) is serialised by s_mtx, taken with a 20 ms timeout so the
 * SeaTalk task can never stall (on timeout the line is dropped and counted).
 * Socket sends use MSG_DONTWAIT: a slow client costs dropped chunks, never
 * blocking. A send error only marks the client dead; the server task closes it.
 * s_line/s_net/s_client are only touched under s_mtx and only when s_line is
 * non-NULL (telnet running); otherwise st_con_printf writes to UART only.
 */
#include "st_con.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "st_con";

#if CONFIG_ST_BRIDGE_TELNET

#include <errno.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "espos_config.h"
#include "lwip/sockets.h"
#include "st_bench.h"

#define LINE_MAX_BYTES 512
#define CON_PORT 23
#define MUTEX_WAIT_MS 20
#define DEBOUNCE_US (500 * 1000)
#define RECHECK_US (200 * 1000)
#define RETRY_US (5 * 1000 * 1000)
#define CFG_NS "bench"
#define CFG_KEY "telnet"

static SemaphoreHandle_t s_mtx; /* created once in st_con_start, never deleted */
static volatile uint32_t s_drops;

static int s_client = -1;           /* written under s_mtx */
static volatile bool s_client_dead; /* set by the sender on a fatal error */
static char *s_line;                /* LINE_MAX_BYTES, guarded by s_mtx; NULL = telnet off */
static char *s_net;                 /* 2*LINE_MAX_BYTES "\n" -> "\r\n" copy, guarded by s_mtx */
static volatile bool s_want;        /* desired state, written by the timer callback */
static volatile bool s_alive;       /* server task exists (set by the starter, cleared by the task) */
static esp_timer_handle_t s_timer;

uint32_t st_con_drops(void)
{
    return s_drops;
}

void st_con_printf(const char *fmt, ...)
{
    va_list ap;
    if (s_mtx == NULL) { /* before st_con_start(): UART only */
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        s_drops++;
        return;
    }

    if (s_line == NULL) { /* telnet off: UART only, still serialised */
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        xSemaphoreGive(s_mtx);
        return;
    }

    va_start(ap, fmt);
    int n = vsnprintf(s_line, LINE_MAX_BYTES, fmt, ap);
    va_end(ap);
    if (n < 0) {
        xSemaphoreGive(s_mtx);
        return;
    }
    if (n >= LINE_MAX_BYTES) {
        memcpy(&s_line[LINE_MAX_BYTES - 4], "...", 4);
        n = LINE_MAX_BYTES - 1;
    }

    if (s_client >= 0 && !s_client_dead) {
        size_t m = 0;
        for (int i = 0; i < n; i++) {
            if (s_line[i] == '\n') {
                s_net[m++] = '\r';
            }
            s_net[m++] = s_line[i];
        }
        const ssize_t w = send(s_client, s_net, m, MSG_DONTWAIT);
        if (w < 0) {
            s_drops++;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM) {
                s_client_dead = true; /* server task closes it */
            }
        } else if ((size_t)w < m) {
            s_drops++; /* rest of the chunk dropped */
        }
    }

    /* UART last, with the mutex still held so lines stay contiguous. 512 bytes
     * at 115200 take ~45 ms; other tasks give up after 20 ms and drop. */
    fwrite(s_line, 1, (size_t)n, stdout);
    xSemaphoreGive(s_mtx);
}

static int listen_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(CON_PORT);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void close_client(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY); /* held only briefly by others */
    if (s_client >= 0) {
        close(s_client);
        s_client = -1;
    }
    s_client_dead = false;
    xSemaphoreGive(s_mtx);
}

/* Telnet IAC filter state: 0 normal, 1 after IAC, 2 skip one option byte,
 * 3 inside subnegotiation (until SE). */
static void feed(const uint8_t *buf, int len, int *st)
{
    for (int i = 0; i < len; i++) {
        const uint8_t c = buf[i];
        switch (*st) {
        case 1: *st = (c >= 251 && c <= 254) ? 2 : (c == 250 ? 3 : 0); break;
        case 2: *st = 0; break;
        case 3:
            if (c == 240) {
                *st = 0;
            }
            break;
        default:
            if (c == 0xFF) {
                *st = 1;
            } else if (c != '\r' && c != '\n' && c != 0) {
                st_bench_key((char)c);
            }
            break;
        }
    }
}

/* Last act of the server task: close the sockets, drop the buffers under
 * s_mtx (st_con_printf sees s_line == NULL afterwards), log, ask the timer to
 * re-check the setting (it may have gone on again meanwhile) and delete the
 * task. */
static void con_exit(int lfd)
{
    if (lfd >= 0) {
        close(lfd);
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_client >= 0) {
        close(s_client);
        s_client = -1;
    }
    s_client_dead = false;
    free(s_line);
    free(s_net);
    s_line = NULL;
    s_net = NULL;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "telnet console stopped, free heap %u", (unsigned)esp_get_free_heap_size());
    s_alive = false;
    esp_timer_stop(s_timer); /* may report "not running": fine */
    esp_timer_start_once(s_timer, RECHECK_US);
    vTaskDelete(NULL);
}

static void con_task(void *arg)
{
    (void)arg;
    int lfd = -1;
    bool logged = false;
    int iac = 0;

    while (s_want) {
        if (lfd < 0) {
            lfd = listen_socket();
            if (lfd < 0) {
                vTaskDelay(pdMS_TO_TICKS(500)); /* network may not be up yet */
                continue;
            }
            if (!logged) {
                ESP_LOGI(TAG, "telnet console listening on port %d", CON_PORT);
                logged = true;
            }
        }

        if (s_client_dead) {
            close_client();
        }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        int maxfd = lfd;
        const int cfd = s_client;
        if (cfd >= 0) {
            FD_SET(cfd, &rd);
            if (cfd > maxfd) {
                maxfd = cfd;
            }
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
        const int r = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (r < 0) {
            ESP_LOGW(TAG, "select failed (errno %d), restarting listener", errno);
            close_client();
            close(lfd);
            lfd = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (r == 0) {
            continue;
        }

        if (FD_ISSET(lfd, &rd)) {
            const int nfd = accept(lfd, NULL, NULL);
            if (nfd >= 0) {
                close_client(); /* a new connection replaces the old one */
                const int one = 1;
                setsockopt(nfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                s_client = nfd;
                s_client_dead = false;
                xSemaphoreGive(s_mtx);
                iac = 0;
                const esp_app_desc_t *d = esp_app_get_description();
                st_con_printf("\nseatalk-bridge %s console, h for help\n", d ? d->version : "?");
                st_bench_print_help();
                continue;
            }
        }
        if (cfd >= 0 && FD_ISSET(cfd, &rd)) {
            uint8_t buf[64];
            const int n = recv(cfd, buf, sizeof buf, MSG_DONTWAIT);
            if (n > 0) {
                feed(buf, n, &iac);
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close_client();
            }
        }
    }
    con_exit(lfd);
}

/* Runs on the esp_timer task: apply the setting. Only this callback starts
 * the server; the server task only ever stops itself. */
static void apply_cb(void *arg)
{
    (void)arg;
    bool on = false;
    (void)espos_config_get_bool(CFG_NS, CFG_KEY, &on);
    s_want = on;
    if (!on || s_alive) {
        return; /* off: the task sees s_want and exits; on and running: nothing to do */
    }

    char *line = malloc(LINE_MAX_BYTES);
    char *net = malloc(2 * LINE_MAX_BYTES);
    if (line == NULL || net == NULL) {
        free(line);
        free(net);
        ESP_LOGE(TAG, "telnet console: out of memory, retrying");
        esp_timer_start_once(s_timer, RETRY_US);
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_line = line;
    s_net = net;
    xSemaphoreGive(s_mtx);
    s_alive = true;
    if (xTaskCreate(con_task, "st_con", 4096, NULL, 3, NULL) != pdPASS) {
        s_alive = false;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_line = NULL;
        s_net = NULL;
        xSemaphoreGive(s_mtx);
        free(line);
        free(net);
        ESP_LOGE(TAG, "telnet console: task create failed, retrying");
        esp_timer_start_once(s_timer, RETRY_US);
        return;
    }
    ESP_LOGI(TAG, "telnet console started, free heap %u", (unsigned)esp_get_free_heap_size());
}

/* Config change callback: only (re)arms the debounce timer, never blocks. */
static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    if (ns && key && strcmp(ns, CFG_NS) == 0 && strcmp(key, CFG_KEY) == 0 && s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_start_once(s_timer, DEBOUNCE_US);
    }
}

void st_con_start(void)
{
    if (s_mtx != NULL) {
        return;
    }
    s_mtx = xSemaphoreCreateMutex();
    const esp_timer_create_args_t ta = {.callback = apply_cb, .name = "st_con"};
    if (s_mtx == NULL || esp_timer_create(&ta, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "telnet console init failed");
        return;
    }
    const esp_err_t err = espos_config_subscribe(on_config_change, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config subscribe failed: %s (telnet setting only read at boot)", esp_err_to_name(err));
    }
    esp_timer_start_once(s_timer, 0); /* initial state */
}

#else /* !CONFIG_ST_BRIDGE_TELNET */

#include "espos_config.h"

uint32_t st_con_drops(void)
{
    return 0;
}

void st_con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void st_con_start(void)
{
    bool on = false;
    if (espos_config_get_bool("bench", "telnet", &on) == ESP_OK && on) {
        ESP_LOGW(TAG, "bench.telnet is on but this firmware has no telnet console (CONFIG_ST_BRIDGE_TELNET)");
    }
}

#endif
