/*
 * Signal K adapter, pure part: path table, own-source check, magnetic
 * fallback and value parsing. No ESP-IDF, no espOS: unit-tested on a PC.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "st_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Quantity <-> Signal K path. LAMP_LEVEL has no row (no Signal K path). */
typedef struct {
    st_quantity_t q;
    const char *path;
    const char *src; /* short name: config key "src_<src>" */
} st_sk_qpath_t;

const st_sk_qpath_t *st_sk_qpaths(size_t *count);
const char *st_sk_path_of(st_quantity_t q);     /* NULL when q has no path */
const char *st_sk_src_name_of(st_quantity_t q); /* "depth", "awa", ... or NULL */

typedef enum {
    ST_SK_ROLE_DIRECT = 0, /* value goes to quantity q as is */
    ST_SK_ROLE_TRUE_HDG,   /* navigation.headingTrue: feeds HEADING_MAGNETIC */
    ST_SK_ROLE_TRUE_COG,   /* navigation.courseOverGroundTrue: feeds COG_MAGNETIC */
    ST_SK_ROLE_VARIATION,  /* navigation.magneticVariation */
} st_sk_role_t;

/* One Signal K subscription. q is the quantity the update feeds (for the
 * true paths the magnetic quantity they can stand in for; ST_Q_NONE for the
 * variation). */
typedef struct {
    const char *path;
    st_quantity_t q;
    st_sk_role_t role;
    uint16_t period_ms;
} st_sk_sub_t;

const st_sk_sub_t *st_sk_subs(size_t *count);

/* Own-source rule: source == hostname or starts with hostname + ".".
 * A NULL source is foreign (false). */
bool st_sk_is_own_source(const char *source, const char *hostname);

/* x wrapped into [0, 2pi). */
double st_sk_wrap_2pi(double x);

/* The path to blank (JSON null) when a datagram reports a defective sensor:
 * cmd 0x00 depth, 0x23 water temperature; NULL for any other cmd. */
const char *st_sk_defective_path(uint8_t cmd);

typedef enum { ST_SK_VAL_NUMBER = 0, ST_SK_VAL_NULL, ST_SK_VAL_BAD } st_sk_val_t;

/* value_json -> number | null | bad (object, string, bool, trailing garbage,
 * NaN/Inf, empty). *out is set only for NUMBER. */
st_sk_val_t st_sk_parse_value(const char *value_json, double *out);

#define ST_SK_NEVER INT64_MIN
#define ST_SK_VARIATION_MAX_AGE_MS (10 * 60 * 1000)

/* Magnetic fallback decision. Times are ms on one clock; ST_SK_NEVER = no
 * value yet. The magnetic value is derived (true - variation, wrapped to
 * 0..2pi into *mag) only when the fallback is enabled, the direct magnetic
 * path has not delivered within stale_ms, and the variation is known and
 * younger than ST_SK_VARIATION_MAX_AGE_MS. */
bool st_sk_derive_mag(bool enabled, int64_t now_ms, double true_v, int64_t mag_ms, double variation,
                      int64_t variation_ms, uint32_t stale_ms, double *mag);

#ifdef __cplusplus
}
#endif
