/*
 * SeaTalk bench-test application: self-test, RX monitor, statistics.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Create the command queue, the SeaTalk task and the UART0 console task. */
void st_bench_start(void);

/* Handle one console key. Only enqueues a command; safe to call from any task
 * (UART console now, network console later). */
void st_bench_key(char c);

#ifdef __cplusplus
}
#endif
