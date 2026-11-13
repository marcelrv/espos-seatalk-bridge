/*
 * Console output/input multiplexer for the bench app: printf-style output goes
 * to UART0 and, when connected, to a telnet client (TCP port 23); bytes from
 * the telnet client are fed to st_bench_key().
 */
#pragma once

#include <stdint.h>

/* Compile-time switch: 0 removes the network console (st_con_printf then only
 * writes to UART). Set to 0 for production builds, see the security note in
 * st_con.c. */
#define ST_CON_ENABLE 1

#ifdef __cplusplus
extern "C" {
#endif

/* Start the telnet server task. Call after espos_start(); safe before the
 * network is up (keeps retrying). */
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
