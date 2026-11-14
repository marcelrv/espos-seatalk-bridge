/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * SeaTalk bench-test application on top of st_core: self-test, RX monitor,
 * simulated boat, per-datagram configuration, statistics.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Register the st_core RX callback and create the bench task (USB serial
 * keys, queued keys, self-test, statistics). Call after st_core_start(). */
void st_bench_start(void);

/* Handle one console key. Only enqueues a command; safe to call from any task
 * (telnet console). */
void st_bench_key(char c);

/* Print the key help via st_con_printf (also the telnet banner). */
void st_bench_print_help(void);

#ifdef __cplusplus
}
#endif
