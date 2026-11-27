/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * One-call start wrapper for espos_seatalk.
 */
#include "espos_seatalk.h"

#include "esp_log.h"
#include "st_sk.h"

#ifdef ESPOS_SEATALK_HAVE_SK
static const char *TAG = "espos_seatalk";
#endif
static bool s_sk_running;

esp_err_t espos_seatalk_start(const st_core_config_t *cfg)
{
    esp_err_t err = st_core_start(cfg);
    if (err != ESP_OK) {
        return err;
    }
#ifdef ESPOS_SEATALK_HAVE_SK
    /* The bus is live from here on: a Signal K adapter failure is reported
     * through espos_seatalk_sk_running(), not as a failed bus start. */
    err = st_sk_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Signal K adapter not started: %s (bus keeps running)", esp_err_to_name(err));
    } else {
        s_sk_running = true;
    }
#endif
    return ESP_OK;
}

bool espos_seatalk_sk_running(void)
{
    return s_sk_running;
}

bool espos_seatalk_has_sk(void)
{
#ifdef ESPOS_SEATALK_HAVE_SK
    return true;
#else
    return false;
#endif
}
