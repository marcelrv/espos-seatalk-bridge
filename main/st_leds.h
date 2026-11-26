/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * RX/TX lights enable pin, driven from seatalk_hw.leds. Does nothing when no
 * enable pin is configured (pin < 0).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drive the validated enable pin from seatalk_hw.leds now and on every change.
 * Pass pin < 0 when the board has no enable pin. Call after espos_start(). */
void st_leds_start(int pin, bool active_high);

#ifdef __cplusplus
}
#endif
