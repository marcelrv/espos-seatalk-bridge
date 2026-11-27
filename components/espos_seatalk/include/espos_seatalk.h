/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_seatalk — one-call entry point for the Raymarine SeaTalk 1 stack and
 * optional espOS Signal K adapter.
 *
 * Usage:
 *   1. Before espos_start(): call st_phy_tx_idle(tx_gpio, cfg.phy.invert) so
 *      the bus stays released during startup (LOW for an inverting interface,
 *      HIGH for a non-inverting one).
 *   2. Start espOS: espos_start(&opts).
 *   3. Start SeaTalk: espos_seatalk_start(&cfg).
 *
 * Threading: espos_seatalk_start() runs on the caller's task (app_main) and
 * starts st_task (and st_sk when built with espOS). Lower-layer headers
 * (st_phy.h, st_link.h, st_codec.h, st_sched.h, st_core.h, st_sk.h) remain
 * public for custom adapters and diagnostics.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "st_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the SeaTalk 1 core task and, when compiled with espOS Signal K
 * support, the Signal K bridge adapter. Call after espos_start() (or directly
 * in standalone ESP-IDF projects without espOS).
 *
 * Returns the core's result only: ESP_OK means the bus is running. A Signal K
 * adapter that fails to start is logged and shows in
 * espos_seatalk_sk_running(); the bus keeps running without it.
 */
esp_err_t espos_seatalk_start(const st_core_config_t *cfg);

/** True once the Signal K adapter has started (always false without espOS). */
bool espos_seatalk_sk_running(void);

/**
 * True when this build includes the espOS Signal K adapter (espos_sk,
 * espos_config, and espos_net present in the project).
 */
bool espos_seatalk_has_sk(void);

#ifdef __cplusplus
}
#endif
