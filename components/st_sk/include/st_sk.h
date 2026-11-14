/*
 * Signal K adapter (spec section 7): the only part that depends on espOS.
 *
 * Bus -> Signal K: every valid foreign datagram whose type's mode forwards
 *   (TO_SK, BOTH, AUTO) is decoded by st_core; each value is published with
 *   espos_sk_publish_number() under this device's source
 *   ("<hostname>.XX" on the server, measured with signalk-server).
 *
 * Signal K -> bus: the adapter subscribes to the Signal K paths of the
 *   quantities st_codec can send and feeds st_core_set_value().
 *   Loop prevention, layer 3: updates whose $source is this device's own
 *   ("<hostname>" or "<hostname>.<anything>") are ignored, so a value that
 *   came from SeaTalk never goes back to SeaTalk. An optional per-quantity
 *   source filter takes only one $source (e.g. the NMEA 2000 wind sensor).
 *   Magnetic fallback: when no fresh headingMagnetic / courseOverGroundMagnetic
 *   arrives, the magnetic value is computed from the true one minus
 *   navigation.magneticVariation (many servers only have the true paths).
 *
 * Config: NVS namespace "seatalk" (config/seatalk.json, edited in the espOS
 *   web UI): per datagram type the mode and period, per quantity the source
 *   filter, plus idle wait, jitter, stale timeout. Applied at start and on
 *   every change, no restart.
 *
 * Diagnostics: every 10 s, espos.<hostname>.seatalk.* (tx/rx counters, bus
 *   load) with metadata.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call after espos_start() and st_core_start(). */
esp_err_t st_sk_start(void);

typedef struct {
    uint32_t published;     /* values sent to Signal K */
    uint32_t received;      /* Signal K updates handed to st_core */
    uint32_t own_ignored;   /* updates with our own $source (loop prevention) */
    uint32_t filtered;      /* updates refused by a source filter */
    uint32_t mag_derived;   /* magnetic values computed from true - variation */
    uint32_t bad_value;     /* updates that weren't a number or null */
} st_sk_stats_t;

void st_sk_get_stats(st_sk_stats_t *out);

/* The $source label this device publishes under (for display and tests). */
const char *st_sk_own_label(void);

#ifdef __cplusplus
}
#endif
