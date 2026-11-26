/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * One-call start wrapper for espos_seatalk.
 */
#include "espos_seatalk.h"

#include "st_sk.h"

esp_err_t espos_seatalk_start(const st_core_config_t *cfg)
{
    esp_err_t err = st_core_start(cfg);
    if (err != ESP_OK) {
        return err;
    }
#ifdef ESPOS_SEATALK_HAVE_SK
    return st_sk_start();
#else
    return ESP_OK;
#endif
}

bool espos_seatalk_has_sk(void)
{
#ifdef ESPOS_SEATALK_HAVE_SK
    return true;
#else
    return false;
#endif
}
