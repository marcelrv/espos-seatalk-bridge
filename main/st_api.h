/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP status endpoint (GET /api/v1/seatalk) on espOS's web server so boat
 * owners can check SeaTalk 1 bus liveness and counters without a serial cable.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register GET /api/v1/seatalk. Call after espos_start() and SeaTalk startup. */
esp_err_t st_api_register(bool running, int tx_pin, int rx_pin, bool invert);

#ifdef __cplusplus
}
#endif
