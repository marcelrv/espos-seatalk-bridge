#include "st_sk_map.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const st_sk_qpath_t k_qpaths[] = {
    {ST_Q_DEPTH_BELOW_TRANSDUCER, "environment.depth.belowTransducer", "depth"},
    {ST_Q_WIND_ANGLE_APPARENT, "environment.wind.angleApparent", "awa"},
    {ST_Q_WIND_SPEED_APPARENT, "environment.wind.speedApparent", "aws"},
    {ST_Q_SPEED_THROUGH_WATER, "navigation.speedThroughWater", "stw"},
    {ST_Q_TRIP_LOG, "navigation.trip.log", "trip"},
    {ST_Q_TOTAL_LOG, "navigation.log", "log"},
    {ST_Q_WATER_TEMPERATURE, "environment.water.temperature", "wtemp"},
    {ST_Q_SPEED_OVER_GROUND, "navigation.speedOverGround", "sog"},
    {ST_Q_COURSE_OVER_GROUND_MAGNETIC, "navigation.courseOverGroundMagnetic", "cog"},
    {ST_Q_HEADING_MAGNETIC, "navigation.headingMagnetic", "hdg"},
    {ST_Q_RUDDER_ANGLE, "steering.rudderAngle", "rudder"},
};
#define N_QPATHS (sizeof k_qpaths / sizeof k_qpaths[0])

static const st_sk_sub_t k_subs[] = {
    {"environment.depth.belowTransducer", ST_Q_DEPTH_BELOW_TRANSDUCER, ST_SK_ROLE_DIRECT, 1000},
    {"environment.wind.angleApparent", ST_Q_WIND_ANGLE_APPARENT, ST_SK_ROLE_DIRECT, 500},
    {"environment.wind.speedApparent", ST_Q_WIND_SPEED_APPARENT, ST_SK_ROLE_DIRECT, 500},
    {"navigation.speedThroughWater", ST_Q_SPEED_THROUGH_WATER, ST_SK_ROLE_DIRECT, 1000},
    {"navigation.trip.log", ST_Q_TRIP_LOG, ST_SK_ROLE_DIRECT, 1000},
    {"navigation.log", ST_Q_TOTAL_LOG, ST_SK_ROLE_DIRECT, 1000},
    {"environment.water.temperature", ST_Q_WATER_TEMPERATURE, ST_SK_ROLE_DIRECT, 1000},
    {"navigation.speedOverGround", ST_Q_SPEED_OVER_GROUND, ST_SK_ROLE_DIRECT, 1000},
    {"navigation.courseOverGroundMagnetic", ST_Q_COURSE_OVER_GROUND_MAGNETIC, ST_SK_ROLE_DIRECT, 1000},
    {"navigation.headingMagnetic", ST_Q_HEADING_MAGNETIC, ST_SK_ROLE_DIRECT, 1000},
    {"steering.rudderAngle", ST_Q_RUDDER_ANGLE, ST_SK_ROLE_DIRECT, 1000},
    {"navigation.headingTrue", ST_Q_HEADING_MAGNETIC, ST_SK_ROLE_TRUE_HDG, 1000},
    {"navigation.courseOverGroundTrue", ST_Q_COURSE_OVER_GROUND_MAGNETIC, ST_SK_ROLE_TRUE_COG, 1000},
    {"navigation.magneticVariation", ST_Q_NONE, ST_SK_ROLE_VARIATION, 1000},
};
#define N_SUBS (sizeof k_subs / sizeof k_subs[0])

const st_sk_qpath_t *st_sk_qpaths(size_t *count)
{
    if (count) {
        *count = N_QPATHS;
    }
    return k_qpaths;
}

const st_sk_sub_t *st_sk_subs(size_t *count)
{
    if (count) {
        *count = N_SUBS;
    }
    return k_subs;
}

static const st_sk_qpath_t *find_q(st_quantity_t q)
{
    for (size_t i = 0; i < N_QPATHS; i++) {
        if (k_qpaths[i].q == q) {
            return &k_qpaths[i];
        }
    }
    return NULL;
}

const char *st_sk_path_of(st_quantity_t q)
{
    const st_sk_qpath_t *p = find_q(q);
    return p ? p->path : NULL;
}

const char *st_sk_src_name_of(st_quantity_t q)
{
    const st_sk_qpath_t *p = find_q(q);
    return p ? p->src : NULL;
}

bool st_sk_is_own_source(const char *source, const char *hostname)
{
    if (!source || !hostname || !*hostname) {
        return false;
    }
    const size_t n = strlen(hostname);
    if (strncmp(source, hostname, n) != 0) {
        return false;
    }
    return source[n] == '\0' || source[n] == '.';
}

double st_sk_wrap_2pi(double x)
{
    const double twopi = 6.283185307179586;
    if (!isfinite(x)) {
        return x;
    }
    x = fmod(x, twopi);
    if (x < 0) {
        x += twopi;
    }
    if (x >= twopi) { /* a tiny negative value plus 2pi rounds up to 2pi */
        x = 0;
    }
    return x;
}

const char *st_sk_defective_path(uint8_t cmd)
{
    switch (cmd) {
    case 0x00: return st_sk_path_of(ST_Q_DEPTH_BELOW_TRANSDUCER);
    case 0x23: return st_sk_path_of(ST_Q_WATER_TEMPERATURE);
    default: return NULL;
    }
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

st_sk_val_t st_sk_parse_value(const char *s, double *out)
{
    if (!s) {
        return ST_SK_VAL_BAD;
    }
    while (is_ws(*s)) {
        s++;
    }
    if (strncmp(s, "null", 4) == 0) {
        const char *t = s + 4;
        while (is_ws(*t)) {
            t++;
        }
        return *t == '\0' ? ST_SK_VAL_NULL : ST_SK_VAL_BAD;
    }
    /* Only JSON number characters: strtod would also take hex, inf, nan. */
    if (*s != '-' && !isdigit((unsigned char)*s)) {
        return ST_SK_VAL_BAD;
    }
    for (const char *c = s; *c; c++) {
        if (!isdigit((unsigned char)*c) && !is_ws(*c) && !strchr("+-.eE", *c)) {
            return ST_SK_VAL_BAD;
        }
    }
    char *end = NULL;
    const double v = strtod(s, &end);
    if (end == s) {
        return ST_SK_VAL_BAD;
    }
    while (is_ws(*end)) {
        end++;
    }
    if (*end != '\0' || !isfinite(v)) {
        return ST_SK_VAL_BAD;
    }
    *out = v;
    return ST_SK_VAL_NUMBER;
}

bool st_sk_derive_mag(bool enabled, int64_t now_ms, double true_v, int64_t mag_ms, double variation,
                      int64_t variation_ms, uint32_t stale_ms, double *mag)
{
    if (!enabled || !isfinite(true_v) || !isfinite(variation) || variation_ms == ST_SK_NEVER) {
        return false;
    }
    if (now_ms - variation_ms > ST_SK_VARIATION_MAX_AGE_MS) {
        return false;
    }
    if (mag_ms != ST_SK_NEVER && now_ms - mag_ms <= (int64_t)stale_ms) {
        return false; /* the direct path is fresh and wins */
    }
    *mag = st_sk_wrap_2pi(true_v - variation);
    return true;
}
