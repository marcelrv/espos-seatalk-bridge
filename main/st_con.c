/*
 * Telnet (TCP port 23) mirror of the bench console.
 *
 * SECURITY: the console is unauthenticated and unencrypted. Anyone on the LAN
 * can send keys (start/stop the simulator, flood the bus). Bench use only;
 * build with ST_CON_ENABLE 0 (st_con.h) for anything else.
 *
 * Threading: one server task owns accept/recv and the lifetime of the client
 * socket. Any task may call st_con_printf(); output (format buffer, socket
 * send, UART write) is serialised by s_mtx, taken with a 20 ms timeout so the
 * SeaTalk task can never stall (on timeout the line is dropped and counted).
 * Socket sends use MSG_DONTWAIT: a slow client costs dropped chunks, never
 * blocking. A send error only marks the client dead; the server task closes it.
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
#include "st_bench.h"

#define LINE_MAX_BYTES 512

static const char *TAG = "st_con";

static SemaphoreHandle_t s_mtx;
static volatile uint32_t s_drops;

uint32_t st_con_drops(void)
{
    return s_drops;
}

#if ST_CON_ENABLE

#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "lwip/sockets.h"

#define CON_PORT 23
#define MUTEX_WAIT_MS 20

static int s_client = -1;           /* written under s_mtx */
static volatile bool s_client_dead; /* set by the sender on a fatal error */
static char s_line[LINE_MAX_BYTES]; /* guarded by s_mtx */
static char s_net[2 * LINE_MAX_BYTES]; /* "\n" -> "\r\n" expanded copy, guarded by s_mtx */

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

    va_start(ap, fmt);
    int n = vsnprintf(s_line, sizeof s_line, fmt, ap);
    va_end(ap);
    if (n < 0) {
        xSemaphoreGive(s_mtx);
        return;
    }
    if (n >= (int)sizeof s_line) {
        memcpy(&s_line[sizeof s_line - 4], "...", 4);
        n = (int)sizeof s_line - 1;
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

static void con_task(void *arg)
{
    (void)arg;
    int lfd = -1;
    bool logged = false;
    int iac = 0;

    for (;;) {
        if (lfd < 0) {
            lfd = listen_socket();
            if (lfd < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000)); /* network may not be up yet */
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
}

void st_con_start(void)
{
    if (s_mtx != NULL) {
        return;
    }
    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL || xTaskCreate(con_task, "st_con", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "telnet console start failed");
    }
}

#else /* !ST_CON_ENABLE */

void st_con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void st_con_start(void)
{
    (void)TAG;
    (void)s_mtx;
    (void)s_drops;
}

#endif
