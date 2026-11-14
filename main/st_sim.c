/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulated boat, after the st50-emulator's Boat struct and sweep formulas.
 * All values are handed to st_core in SI units.
 */
#include "st_sim.h"

#include <math.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st_codec.h"
#include "st_core.h"

static const char *TAG = "st_sim";

#define SIM_PERIOD_MS 500
#define LOG_SPEEDUP 10.0 /* trip/total run this much faster than real time in sweep mode */
#define KN_TO_MS (1852.0 / 3600.0)
#define NM_TO_M 1852.0
#define DEG_TO_RAD (M_PI / 180.0)

typedef struct {
    double depth_m;
    double stw_kn;
    double awa_deg; /* 0..360 relative to the bow, clockwise */
    double aws_kn;
    double water_c;
    double trip_nm;
    double total_nm;
    double sog_kn;
    double cog_deg;
    double heading_deg;
} boat_t;

/* Emulator static values, except heading 235 (odd degree: tests the 89/9C
 * odd-degree flag) and COG 123. */
static const boat_t STATIC_BOAT = {12.3, 5.67, 45.0, 12.3, 18.5, 12.34, 1234.5, 6.2, 123.0, 235.0};

static boat_t s_boat = {12.3, 5.67, 45.0, 12.3, 18.5, 12.34, 1234.5, 6.2, 123.0, 235.0};
static volatile bool s_enabled;
static volatile bool s_sweep;

static void update_boat(double dt_s)
{
    if (!s_sweep) {
        s_boat = STATIC_BOAT;
        return;
    }
    const double t = (double)esp_timer_get_time() / 1e6;
    s_boat.depth_m = 8.0 + 6.0 * sin(t / 30.0);
    s_boat.stw_kn = 5.0 + 2.0 * sin(t / 20.0);
    s_boat.awa_deg = fmod(t * 3.0, 360.0);
    s_boat.aws_kn = 12.0 + 6.0 * sin(t / 25.0);
    s_boat.water_c = 15.0 + 5.0 * sin(t / 60.0);
    s_boat.heading_deg = fmod(t * 2.0, 360.0);
    s_boat.cog_deg = fmod(s_boat.heading_deg + 5.0, 360.0);
    s_boat.sog_kn = s_boat.stw_kn + 0.5;
    const double d_nm = s_boat.stw_kn * dt_s / 3600.0 * LOG_SPEEDUP;
    s_boat.trip_nm += d_nm;
    s_boat.total_nm += d_nm;
    if (s_boat.trip_nm > 999.0) {
        s_boat.trip_nm = 0;
    }
}

static void publish(void)
{
    const double awa = s_boat.awa_deg > 180.0 ? s_boat.awa_deg - 360.0 : s_boat.awa_deg;
    st_core_set_value(ST_Q_DEPTH_BELOW_TRANSDUCER, s_boat.depth_m);
    st_core_set_value(ST_Q_WIND_ANGLE_APPARENT, awa * DEG_TO_RAD);
    st_core_set_value(ST_Q_WIND_SPEED_APPARENT, s_boat.aws_kn * KN_TO_MS);
    st_core_set_value(ST_Q_SPEED_THROUGH_WATER, s_boat.stw_kn * KN_TO_MS);
    st_core_set_value(ST_Q_TRIP_LOG, s_boat.trip_nm * NM_TO_M);
    st_core_set_value(ST_Q_TOTAL_LOG, s_boat.total_nm * NM_TO_M);
    st_core_set_value(ST_Q_WATER_TEMPERATURE, s_boat.water_c + 273.15);
    st_core_set_value(ST_Q_SPEED_OVER_GROUND, s_boat.sog_kn * KN_TO_MS);
    st_core_set_value(ST_Q_COURSE_OVER_GROUND_MAGNETIC, s_boat.cog_deg * DEG_TO_RAD);
    st_core_set_value(ST_Q_HEADING_MAGNETIC, s_boat.heading_deg * DEG_TO_RAD);
}

static void sim_task(void *arg)
{
    (void)arg;
    int64_t last_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SIM_PERIOD_MS));
        const int64_t now = esp_timer_get_time();
        double dt = (double)(now - last_us) / 1e6;
        last_us = now;
        if (!s_enabled) {
            continue; /* values go stale after 5 s: the slots fall silent */
        }
        if (dt > 2.0) {
            dt = SIM_PERIOD_MS / 1000.0;
        }
        update_boat(dt);
        publish();
    }
}

void st_sim_start(void)
{
    if (xTaskCreate(sim_task, "st_sim", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "sim task create failed");
    }
}

void st_sim_enable(bool on)
{
    s_enabled = on;
}

bool st_sim_enabled(void)
{
    return s_enabled;
}

void st_sim_set_sweep(bool sweep)
{
    s_sweep = sweep;
}

bool st_sim_sweep(void)
{
    return s_sweep;
}
