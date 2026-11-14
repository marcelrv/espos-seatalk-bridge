/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * Console output/input multiplexer for the bench app: printf-style output goes
 * to UART0 and, when the telnet console is on and a client is connected, to
 * that client (TCP port 23); bytes from the client are fed to st_bench_key().
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call after espos_start(). With CONFIG_ST_BRIDGE_TELNET it watches the
 * config setting seatalk_console.telnet and starts/stops the telnet server on demand
 * (nothing is allocated while it is off); without it st_con_printf only
 * writes to UART. */
void st_con_start(void);

/* printf to UART and to the telnet client. Lines up to ~512 bytes (longer ones
 * are truncated with "..."). Never blocks for long and is safe to call from
 * any task, including the SeaTalk task. */
void st_con_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Chunks/lines dropped (slow or failing telnet client, or output lock timeout). */
uint32_t st_con_drops(void);

#ifdef __cplusplus
}
#endif
