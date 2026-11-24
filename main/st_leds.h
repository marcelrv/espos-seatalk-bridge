/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * RX/TX lights enable pin (CONFIG_ST_BRIDGE_LED_EN_GPIO), driven from the
 * espOS setting seatalk.leds. Does nothing on a board without that pin.
 */
#pragma once

/* Drive the pin from the setting now and on every change. After espos_start. */
void st_leds_start(void);
