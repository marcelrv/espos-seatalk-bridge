/* SeaTalk 1 codec: see include/st_codec.h for the contract and sources. */
#include "st_codec.h"

#include <math.h>
#include <stddef.h>

#define ST_PI 3.14159265358979323846
#define ST_FT_M 0.3048
#define ST_KN_MS (1852.0 / 3600.0)
#define ST_NM_M 1852.0
#define ST_MPH_MS 0.44704
#define ST_K_OFFSET 273.15

#define ODD_53 0x8
#define ODD_89 0x8 /* Knauf: (U & 0xC) / 8; not the emulator's 0x4 (see header) */
#define ODD_9C 0x4

static const st_dgram_info_t k_table[] = {
    {0x00, 5, "depth", true, true, {ST_Q_DEPTH_BELOW_TRANSDUCER, ST_Q_NONE}, 1000},
    {0x10, 4, "wind_angle", true, true, {ST_Q_WIND_ANGLE_APPARENT, ST_Q_NONE}, 500},
    {0x11, 4, "wind_speed", true, true, {ST_Q_WIND_SPEED_APPARENT, ST_Q_NONE}, 500},
    {0x20, 4, "stw", true, true, {ST_Q_SPEED_THROUGH_WATER, ST_Q_NONE}, 1000},
    {0x21, 5, "trip", true, true, {ST_Q_TRIP_LOG, ST_Q_NONE}, 0},
    {0x22, 5, "total_log", true, true, {ST_Q_TOTAL_LOG, ST_Q_NONE}, 0},
    {0x23, 4, "water_temp_int", true, true, {ST_Q_WATER_TEMPERATURE, ST_Q_NONE}, 0},
    {0x25, 7, "log", true, true, {ST_Q_TOTAL_LOG, ST_Q_TRIP_LOG}, 5000},
    {0x26, 7, "stw_precise", true, true, {ST_Q_SPEED_THROUGH_WATER, ST_Q_NONE}, 0},
    {0x27, 4, "water_temp", true, true, {ST_Q_WATER_TEMPERATURE, ST_Q_NONE}, 5000},
    {0x30, 3, "lamp", true, false, {ST_Q_LAMP_LEVEL, ST_Q_NONE}, 0},
    {0x52, 4, "sog", true, true, {ST_Q_SPEED_OVER_GROUND, ST_Q_NONE}, 1000},
    {0x53, 3, "cog", true, true, {ST_Q_COURSE_OVER_GROUND_MAGNETIC, ST_Q_NONE}, 1000},
    {0x89, 5, "compass", true, true, {ST_Q_HEADING_MAGNETIC, ST_Q_NONE}, 1000}, /* ST50 reads heading from 89 (measured) */
    {0x9C, 4, "heading_rudder", true, true, {ST_Q_HEADING_MAGNETIC, ST_Q_RUDDER_ANGLE}, 0}, /* autopilot style; ST50 ignores it */
};

#define TABLE_N (sizeof(k_table) / sizeof(k_table[0]))

const st_dgram_info_t *st_codec_info(uint8_t cmd)
{
    for (size_t i = 0; i < TABLE_N; i++) {
        if (k_table[i].cmd == cmd) return &k_table[i];
    }
    return NULL;
}

const st_dgram_info_t *st_codec_table(uint8_t *count)
{
    if (count) *count = (uint8_t)TABLE_N;
    return k_table;
}

const char *st_quantity_name(st_quantity_t q)
{
    static const char *const names[ST_Q_COUNT] = {
        "none",
        "depth_below_transducer",
        "wind_angle_apparent",
        "wind_speed_apparent",
        "speed_through_water",
        "trip_log",
        "total_log",
        "water_temperature",
        "speed_over_ground",
        "course_over_ground_magnetic",
        "heading_magnetic",
        "rudder_angle",
        "lamp_level",
    };
    if ((int)q < 0 || (int)q >= (int)ST_Q_COUNT) return "?";
    return names[q];
}

/* ---------------------------------------------------------------- decode */

static void add(st_decoded_t *o, st_quantity_t q, double v)
{
    o->v[o->n].q = q;
    o->v[o->n].value = v;
    o->n++;
}

static unsigned le16(const uint8_t *b) { return (unsigned)b[0] | ((unsigned)b[1] << 8); }

static double deg_to_rad(double deg) { return deg * ST_PI / 180.0; }

/* Heading/course in whole degrees, kept modulo 360, as radians 0..2pi. */
static double heading_rad(int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;
    return deg_to_rad((double)deg);
}

/* 53 / 89 share this layout (Knauf). */
static int heading_deg_53(const uint8_t *b)
{
    int u = b[1] >> 4;
    int vw = b[2];
    return (u & 0x3) * 90 + (vw & 0x3F) * 2 + (u & 0xC) / 8;
}

st_dec_status_t st_codec_decode(const uint8_t *b, uint8_t len, st_decoded_t *out)
{
    const st_dgram_info_t *info;
    if (!b || !out || len < 1) return ST_DEC_UNKNOWN;
    out->n = 0;
    out->flags = 0;
    out->v[0].q = out->v[1].q = ST_Q_NONE;
    out->v[0].value = out->v[1].value = 0.0;

    info = st_codec_info(b[0]);
    if (!info || !info->can_decode) return ST_DEC_UNKNOWN;
    if (len != info->len) return ST_DEC_BAD_LENGTH;

    switch (b[0]) {
    case 0x00: {
        int y = b[2] >> 4, z = b[2] & 0x0F;
        if (y & 8) out->flags |= ST_DF_ANCHOR_ALARM;
        if (y & 4) out->flags |= ST_DF_DISPLAY_METRIC;
        if (z & 2) out->flags |= ST_DF_DEEP_ALARM;
        if (z & 1) out->flags |= ST_DF_SHALLOW_ALARM;
        if (z & 4) {
            out->flags |= ST_DF_SENSOR_DEFECTIVE;
        } else {
            add(out, ST_Q_DEPTH_BELOW_TRANSDUCER, (double)le16(&b[3]) / 10.0 * ST_FT_M);
        }
        break;
    }
    case 0x10: {
        double deg = (double)(((unsigned)b[2] << 8) | b[3]) / 2.0;
        if (deg > 180.0) deg -= 360.0;
        add(out, ST_Q_WIND_ANGLE_APPARENT, deg_to_rad(deg));
        break;
    }
    case 0x11: {
        double v = (double)(b[2] & 0x7F) + (double)(b[3] & 0x0F) / 10.0;
        if (b[2] & 0x80) {
            out->flags |= ST_DF_UNIT_MS;
            add(out, ST_Q_WIND_SPEED_APPARENT, v);
        } else {
            add(out, ST_Q_WIND_SPEED_APPARENT, v * ST_KN_MS);
        }
        break;
    }
    case 0x20:
        add(out, ST_Q_SPEED_THROUGH_WATER, (double)le16(&b[2]) / 10.0 * ST_KN_MS);
        break;
    case 0x21: {
        unsigned v = (unsigned)b[2] | ((unsigned)b[3] << 8) | ((unsigned)(b[4] & 0x0F) << 16);
        add(out, ST_Q_TRIP_LOG, (double)v / 100.0 * ST_NM_M);
        break;
    }
    case 0x22:
        add(out, ST_Q_TOTAL_LOG, (double)le16(&b[2]) / 10.0 * ST_NM_M);
        break;
    case 0x23:
        if ((b[1] >> 4) & 4) {
            out->flags |= ST_DF_SENSOR_DEFECTIVE;
        } else {
            add(out, ST_Q_WATER_TEMPERATURE, (double)(int8_t)b[2] + ST_K_OFFSET);
        }
        break;
    case 0x25: {
        unsigned z = (unsigned)(b[1] >> 4);
        unsigned tot = (unsigned)b[2] | ((unsigned)b[3] << 8) | (z << 16);
        unsigned trp = (unsigned)b[4] | ((unsigned)b[5] << 8) | ((unsigned)(b[6] & 0x0F) << 16);
        add(out, ST_Q_TOTAL_LOG, (double)tot / 10.0 * ST_NM_M);
        add(out, ST_Q_TRIP_LOG, (double)trp / 100.0 * ST_NM_M);
        break;
    }
    case 0x26: {
        int d = b[6] >> 4, e = b[6] & 0x0F;
        if (e & 2) out->flags |= ST_DF_UNIT_MPH;
        if (!(d & 4)) {
            out->flags |= ST_DF_INVALID;
        } else {
            double v = (double)le16(&b[2]) / 100.0;
            add(out, ST_Q_SPEED_THROUGH_WATER, v * ((e & 2) ? ST_MPH_MS : ST_KN_MS));
        }
        break;
    }
    case 0x27:
        add(out, ST_Q_WATER_TEMPERATURE,
            ((double)le16(&b[2]) - 100.0) / 10.0 + ST_K_OFFSET);
        break;
    case 0x30:
        add(out, ST_Q_LAMP_LEVEL, (double)((b[2] & 0x0C) >> 2));
        break;
    case 0x52:
        add(out, ST_Q_SPEED_OVER_GROUND, (double)le16(&b[2]) / 10.0 * ST_KN_MS);
        break;
    case 0x53:
        add(out, ST_Q_COURSE_OVER_GROUND_MAGNETIC, heading_rad(heading_deg_53(b)));
        break;
    case 0x89:
        add(out, ST_Q_HEADING_MAGNETIC, heading_rad(heading_deg_53(b)));
        break;
    case 0x9C: {
        int u = b[1] >> 4, vw = b[2];
        int odd = (u & 0xC) ? (((u & 0xC) == 0xC) ? 2 : 1) : 0;
        int deg = (u & 0x3) * 90 + (vw & 0x3F) * 2 + odd;
        if (u & 8) out->flags |= ST_DF_TURNING_RIGHT;
        add(out, ST_Q_HEADING_MAGNETIC, heading_rad(deg));
        add(out, ST_Q_RUDDER_ANGLE, deg_to_rad((double)(int8_t)b[3]));
        break;
    }
    default:
        return ST_DEC_UNKNOWN;
    }
    return ST_DEC_OK;
}

/* ---------------------------------------------------------------- encode */

/* Round to nearest, halves away from zero, with a tiny bias so that values
 * like 18.5 that went through SI conversions still round as intended. */
static long long rnd(double x)
{
    return llround(x + (x < 0 ? -1e-9 : 1e-9));
}

/* Round and clamp to 0..max; NaN and negatives give 0. */
static unsigned clampu(double x, unsigned max)
{
    if (!(x > 0.0)) return 0;
    if (x >= (double)max) return max;
    {
        long long r = rnd(x);
        if (r < 0) return 0;
        if (r > (long long)max) return max;
        return (unsigned)r;
    }
}

static double rad_to_deg(double rad) { return rad * 180.0 / ST_PI; }

/* Degrees normalised into [0, 360). Non-finite input gives 0. */
static double deg_norm(double rad)
{
    double d;
    if (!isfinite(rad)) return 0.0;
    d = fmod(rad_to_deg(rad), 360.0);
    if (d < 0.0) d += 360.0;
    return d;
}

/* Heading nibble/byte pair per the emulator's encodeHeading. */
static void enc_heading(double rad, unsigned odd_flag, uint8_t *u_out, uint8_t *vw_out)
{
    unsigned h = (unsigned)(rnd(deg_norm(rad)) % 360);
    unsigned u = h / 90, rem = h % 90;
    unsigned vw = (rem / 2) & 0x3F;
    if (rem & 1) u |= odd_flag;
    *u_out = (uint8_t)u;
    *vw_out = (uint8_t)vw;
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/* A missing or non-finite (NaN, inf) required value keeps the slot silent:
 * a display must go blank rather than show 0 (spec 5.2 rule 7). */
#define NEED(q, var)                                                 \
    do {                                                             \
        if (!get((q), &(var), arg) || !isfinite(var)) return 0;      \
    } while (0)

uint8_t st_codec_encode(uint8_t cmd, st_value_get_fn get, void *arg, uint8_t *out)
{
    const st_dgram_info_t *info = st_codec_info(cmd);
    double a = 0.0, b2 = 0.0;
    if (!info || !info->can_encode || !get || !out) return 0;

    switch (cmd) {
    case 0x00:
        NEED(ST_Q_DEPTH_BELOW_TRANSDUCER, a);
        out[1] = 0x02;
        out[2] = 0x00;
        put16(&out[3], clampu(a / ST_FT_M * 10.0, 0xFFFF));
        break;
    case 0x10: {
        unsigned v;
        NEED(ST_Q_WIND_ANGLE_APPARENT, a);
        v = (unsigned)(rnd(deg_norm(a) * 2.0) % 720);
        out[1] = 0x01;
        out[2] = (uint8_t)(v >> 8);
        out[3] = (uint8_t)(v & 0xFF);
        break;
    }
    case 0x11: {
        unsigned tenths, whole;
        NEED(ST_Q_WIND_SPEED_APPARENT, a);
        tenths = clampu(a / ST_KN_MS * 10.0, 0xFFFFFF);
        whole = tenths / 10;
        out[1] = 0x01;
        if (whole > 0x7F) {
            out[2] = 0x7F;
            out[3] = 9;
        } else {
            out[2] = (uint8_t)whole;
            out[3] = (uint8_t)(tenths % 10);
        }
        break;
    }
    case 0x20:
        NEED(ST_Q_SPEED_THROUGH_WATER, a);
        out[1] = 0x01;
        put16(&out[2], clampu(a / ST_KN_MS * 10.0, 0xFFFF));
        break;
    case 0x21: {
        unsigned v;
        NEED(ST_Q_TRIP_LOG, a);
        v = clampu(a / ST_NM_M * 100.0, 0xFFFFF);
        out[1] = 0x02;
        put16(&out[2], v & 0xFFFF);
        out[4] = (uint8_t)((v >> 16) & 0x0F);
        break;
    }
    case 0x22:
        NEED(ST_Q_TOTAL_LOG, a);
        out[1] = 0x02;
        put16(&out[2], clampu(a / ST_NM_M * 10.0, 0xFFFF));
        out[4] = 0x00;
        break;
    case 0x23: {
        double c, f;
        long long ci;
        NEED(ST_Q_WATER_TEMPERATURE, a);
        c = a - ST_K_OFFSET;
        f = c * 9.0 / 5.0 + 32.0;
        if (!(c > -128.0)) ci = -128;
        else if (c >= 127.0) ci = 127;
        else ci = rnd(c);
        out[1] = 0x01;
        out[2] = (uint8_t)(int8_t)ci;
        out[3] = (uint8_t)clampu(f, 255);
        break;
    }
    case 0x25: {
        unsigned tot, trp;
        NEED(ST_Q_TOTAL_LOG, a);
        NEED(ST_Q_TRIP_LOG, b2);
        tot = clampu(a / ST_NM_M * 10.0, 0xFFFFF);
        trp = clampu(b2 / ST_NM_M * 100.0, 0xFFFFF);
        out[1] = (uint8_t)((((tot >> 16) & 0xF) << 4) | 0x04);
        put16(&out[2], tot & 0xFFFF);
        put16(&out[4], trp & 0xFFFF);
        out[6] = (uint8_t)((trp >> 16) & 0x0F);
        break;
    }
    case 0x26: {
        unsigned v;
        NEED(ST_Q_SPEED_THROUGH_WATER, a);
        v = clampu(a / ST_KN_MS * 100.0, 0xFFFF);
        out[1] = 0x04;
        put16(&out[2], v);
        put16(&out[4], v);
        out[6] = 0x40;
        break;
    }
    case 0x27:
        NEED(ST_Q_WATER_TEMPERATURE, a);
        out[1] = 0x01;
        put16(&out[2], clampu((a - ST_K_OFFSET) * 10.0 + 100.0, 0xFFFF));
        break;
    case 0x52:
        NEED(ST_Q_SPEED_OVER_GROUND, a);
        out[1] = 0x01;
        put16(&out[2], clampu(a / ST_KN_MS * 10.0, 0xFFFF));
        break;
    case 0x53: {
        uint8_t u, vw;
        NEED(ST_Q_COURSE_OVER_GROUND_MAGNETIC, a);
        enc_heading(a, ODD_53, &u, &vw);
        out[1] = (uint8_t)(u << 4);
        out[2] = vw;
        break;
    }
    case 0x89: {
        uint8_t u, vw;
        NEED(ST_Q_HEADING_MAGNETIC, a);
        enc_heading(a, ODD_89, &u, &vw);
        out[1] = (uint8_t)((u << 4) | 0x02);
        out[2] = vw;
        out[3] = 0x00;
        out[4] = 0x20;
        break;
    }
    case 0x9C: {
        uint8_t u, vw;
        int r = 0;
        NEED(ST_Q_HEADING_MAGNETIC, a);
        if (get(ST_Q_RUDDER_ANGLE, &b2, arg) && isfinite(b2)) {
            double d = rad_to_deg(b2);
            if (d > 127.0) d = 127.0;
            if (d < -127.0) d = -127.0;
            r = (int)rnd(d);
            if (r > 127) r = 127;
            if (r < -127) r = -127;
        }
        enc_heading(a, ODD_9C, &u, &vw);
        out[1] = (uint8_t)((u << 4) | 0x01);
        out[2] = vw;
        out[3] = (uint8_t)(int8_t)r;
        break;
    }
    default:
        return 0;
    }
    out[0] = cmd;
    return info->len;
}
