/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulated boat for the bench: feeds st_core_set_value() every 500 ms while
 * enabled. Disabled, it stops updating, so the values go stale (stale test).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the simulator task (disabled, STATIC mode). */
void st_sim_start(void);

void st_sim_enable(bool on);
bool st_sim_enabled(void);

/* true = SWEEP (moving values), false = STATIC (fixed, easy to recognise). */
void st_sim_set_sweep(bool sweep);
bool st_sim_sweep(void);

#ifdef __cplusplus
}
#endif
