/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * GET /api/v1/seatalk — lightweight JSON bus health & counter endpoint.
 * Runs on the esp_http_server task; reads only non-blocking snapshots from
 * st_core and st_sk.
 */
#include "st_api.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "espos_httpd.h"
#include "st_core.h"
#include "st_sk.h"

typedef struct {
    bool running;
    int tx_pin;
    int rx_pin;
    bool invert;
} st_api_ctx_t;

static st_api_ctx_t s_ctx;

static esp_err_t handle_get_seatalk(httpd_req_t *req)
{
    const st_api_ctx_t *ctx = (const st_api_ctx_t *)req->user_ctx;
    st_core_stats_t cs;
    st_sk_stats_t ss;
    memset(&cs, 0, sizeof cs);
    memset(&ss, 0, sizeof ss);

    if (ctx->running) {
        st_core_get_stats(&cs);
        st_sk_get_stats(&ss);
    }

    const uint32_t rx_errors = cs.phy.frame_err + cs.phy.hw_overflow + cs.phy.ring_overflow +
                               cs.link.incomplete + cs.link.flagged;

    char buf[512];
    const int n = snprintf(
        buf, sizeof buf,
        "{\"running\":%s,\"tx_pin\":%d,\"rx_pin\":%d,\"invert\":%s,"
        "\"bus_held_low\":%s,\"no_echo_pause\":%s,\"paused\":%s,"
        "\"rx_datagrams\":%" PRIu32 ",\"rx_bytes\":%" PRIu32 ",\"rx_errors\":%" PRIu32 ","
        "\"tx_ok\":%" PRIu32 ",\"tx_collisions\":%" PRIu32 ",\"tx_dropped\":%" PRIu32 ","
        "\"sk_published\":%" PRIu32 ",\"sk_received\":%" PRIu32 ","
        "\"sk_own_ignored\":%" PRIu32 ",\"sk_filtered\":%" PRIu32 "}",
        ctx->running ? "true" : "false", ctx->tx_pin, ctx->rx_pin, ctx->invert ? "true" : "false",
        cs.bus_held_low ? "true" : "false", cs.no_echo_pause ? "true" : "false",
        cs.paused ? "true" : "false", cs.link.datagrams, cs.phy.rx_bytes, rx_errors, cs.tx_ok,
        cs.tx_collisions, cs.tx_dropped, ss.published, ss.received, ss.own_ignored, ss.filtered);

    if (n < 0 || (size_t)n >= sizeof buf) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "format_error",
                                      "status JSON truncated");
    }
    return espos_httpd_send_json(req, NULL, buf);
}

esp_err_t st_api_register(bool running, int tx_pin, int rx_pin, bool invert)
{
    s_ctx.running = running;
    s_ctx.tx_pin = tx_pin;
    s_ctx.rx_pin = rx_pin;
    s_ctx.invert = invert;

    const httpd_uri_t uri = {
        .uri = "/api/v1/seatalk",
        .method = HTTP_GET,
        .handler = handle_get_seatalk,
        .user_ctx = &s_ctx,
    };
    return espos_httpd_register(&uri);
}
