/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 */
/* Host unit tests for the SeaTalk 1 codec. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "st_codec.h"
#include "test_util.h"

#define ST_PI 3.14159265358979323846
#define FT 0.3048
#define KN (1852.0 / 3600.0)
#define NM 1852.0
#define MPH 0.44704
#define DEG (ST_PI / 180.0)
#define KELVIN 273.15

#define CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                         \
        const double va_ = (double)(a);                                          \
        const double vb_ = (double)(b);                                          \
        if (!(fabs(va_ - vb_) <= (double)(eps))) {                               \
            printf("%s:%d: CHECK_NEAR failed: %s ~ %s\n    expected %.9g, actual %.9g (eps %g)\n", \
                   __FILE__, __LINE__, #a, #b, vb_, va_, (double)(eps));         \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- value source ---------------------------------------------------- */

typedef struct {
    bool has[ST_Q_COUNT];
    double v[ST_Q_COUNT];
} vals_t;

static bool get_fn(st_quantity_t q, double *value, void *arg)
{
    const vals_t *s = (const vals_t *)arg;
    if ((int)q <= 0 || (int)q >= (int)ST_Q_COUNT || !s->has[q]) return false;
    *value = s->v[q];
    return true;
}

static void set(vals_t *s, st_quantity_t q, double v)
{
    s->has[q] = true;
    s->v[q] = v;
}

static uint8_t enc1(uint8_t cmd, st_quantity_t q, double v, uint8_t *out)
{
    vals_t s;
    memset(&s, 0, sizeof s);
    set(&s, q, v);
    return st_codec_encode(cmd, get_fn, &s, out);
}

static st_dec_status_t dec(const uint8_t *b, size_t len, st_decoded_t *d)
{
    return st_codec_decode(b, (uint8_t)len, d);
}

/* Encode with one quantity, compare against the literal bytes, return nothing. */
#define EXPECT_BYTES(cmd, q, val, lit)                                           \
    do {                                                                         \
        uint8_t eo_[18];                                                          \
        uint8_t en_ = enc1((cmd), (q), (val), eo_);                                \
        CHECK_EQ_INT(en_, sizeof(lit));                                           \
        if (en_ == sizeof(lit)) CHECK_EQ_MEM(eo_, lit, sizeof(lit));               \
    } while (0)

/* ---- 1. static boat vectors ------------------------------------------ */

static void test_static_boat_vectors(void)
{
    st_decoded_t d;
    vals_t s;

    /* depth: 12.3/0.3048*10 = 403.54 -> 404 = 0x0194 */
    static const uint8_t depth[] = {0x00, 0x02, 0x00, 0x94, 0x01};
    /* stw: 5.67 kn * 10 = 56.7 -> 57 = 0x0039 */
    static const uint8_t stw[] = {0x20, 0x01, 0x39, 0x00};
    /* awa: 45 deg * 2 = 90 = 0x005A, big endian */
    static const uint8_t awa[] = {0x10, 0x01, 0x00, 0x5A};
    /* aws: 12.3 kn -> tenths 123 -> b2 = 12 = 0x0C, b3 = 3 */
    static const uint8_t aws[] = {0x11, 0x01, 0x0C, 0x03};
    /* water 18.5 C: 27: 18.5*10 + 100 = 285 = 0x011D */
    static const uint8_t wt27[] = {0x27, 0x01, 0x1D, 0x01};
    /* 23: XX = round(18.5) = 19 = 0x13; YY = 18.5*9/5+32 = 65.3 -> 65 = 0x41 */
    static const uint8_t wt23[] = {0x23, 0x01, 0x13, 0x41};
    /* trip 12.34 nm *100 = 1234 = 0x04D2 */
    static const uint8_t trip[] = {0x21, 0x02, 0xD2, 0x04, 0x00};
    /* total 1234.5 nm *10 = 12345 = 0x3039 */
    static const uint8_t total[] = {0x22, 0x02, 0x39, 0x30, 0x00};
    /* log: total 12345 = 0x003039 (Z=0, b1 = 0x04), trip 1234 = 0x0004D2, b6 = 0 */
    static const uint8_t log25[] = {0x25, 0x04, 0x39, 0x30, 0xD2, 0x04, 0x00};
    /* sog 6.2 kn * 10 = 62 = 0x3E */
    static const uint8_t sog[] = {0x52, 0x01, 0x3E, 0x00};
    /* cog 123: u = 1, rem = 33, vw = 16 = 0x10, odd -> u = 1|8 = 9 */
    static const uint8_t cog[] = {0x53, 0x90, 0x10};
    /* heading 234: u = 2, rem = 54, vw = 27 = 0x1B, even */
    static const uint8_t h53[] = {0x53, 0x20, 0x1B};
    static const uint8_t h89[] = {0x89, 0x22, 0x1B, 0x00, 0x20};
    /* 9C: b1 = (2<<4)|1 = 0x21; no rudder -> 0 */
    static const uint8_t h9c[] = {0x9C, 0x21, 0x1B, 0x00};

    EXPECT_BYTES(0x00, ST_Q_DEPTH_BELOW_TRANSDUCER, 12.3, depth);
    EXPECT_BYTES(0x20, ST_Q_SPEED_THROUGH_WATER, 5.67 * KN, stw);
    EXPECT_BYTES(0x10, ST_Q_WIND_ANGLE_APPARENT, 45 * DEG, awa);
    EXPECT_BYTES(0x11, ST_Q_WIND_SPEED_APPARENT, 12.3 * KN, aws);
    EXPECT_BYTES(0x27, ST_Q_WATER_TEMPERATURE, 18.5 + KELVIN, wt27);
    EXPECT_BYTES(0x23, ST_Q_WATER_TEMPERATURE, 18.5 + KELVIN, wt23);
    EXPECT_BYTES(0x21, ST_Q_TRIP_LOG, 12.34 * NM, trip);
    EXPECT_BYTES(0x22, ST_Q_TOTAL_LOG, 1234.5 * NM, total);
    EXPECT_BYTES(0x52, ST_Q_SPEED_OVER_GROUND, 6.2 * KN, sog);
    EXPECT_BYTES(0x53, ST_Q_COURSE_OVER_GROUND_MAGNETIC, 123 * DEG, cog);
    EXPECT_BYTES(0x53, ST_Q_COURSE_OVER_GROUND_MAGNETIC, 234 * DEG, h53);
    EXPECT_BYTES(0x89, ST_Q_HEADING_MAGNETIC, 234 * DEG, h89);
    EXPECT_BYTES(0x9C, ST_Q_HEADING_MAGNETIC, 234 * DEG, h9c);

    memset(&s, 0, sizeof s);
    set(&s, ST_Q_TOTAL_LOG, 1234.5 * NM);
    set(&s, ST_Q_TRIP_LOG, 12.34 * NM);
    {
        uint8_t o[18];
        uint8_t n = st_codec_encode(0x25, get_fn, &s, o);
        CHECK_EQ_INT(n, sizeof log25);
        if (n == sizeof log25) CHECK_EQ_MEM(o, log25, sizeof log25);
    }

    /* decode back, within half an LSB */
    CHECK_EQ_INT(dec(depth, sizeof depth, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.n, 1);
    CHECK_EQ_INT(d.v[0].q, ST_Q_DEPTH_BELOW_TRANSDUCER);
    CHECK_NEAR(d.v[0].value, 12.3, 0.05 * FT + 1e-9);

    CHECK_EQ_INT(dec(stw, sizeof stw, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 5.67 * KN, 0.05 * KN + 1e-9);

    CHECK_EQ_INT(dec(awa, sizeof awa, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 45 * DEG, 0.25 * DEG + 1e-9);

    CHECK_EQ_INT(dec(aws, sizeof aws, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 12.3 * KN, 0.05 * KN + 1e-9);

    CHECK_EQ_INT(dec(wt27, sizeof wt27, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 18.5 + KELVIN, 0.05 + 1e-9);
    CHECK_EQ_INT(dec(wt23, sizeof wt23, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 18.5 + KELVIN, 0.5 + 1e-9);

    CHECK_EQ_INT(dec(trip, sizeof trip, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 12.34 * NM, 0.005 * NM + 1e-6);
    CHECK_EQ_INT(dec(total, sizeof total, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 1234.5 * NM, 0.05 * NM + 1e-6);

    CHECK_EQ_INT(dec(log25, sizeof log25, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.n, 2);
    CHECK_EQ_INT(d.v[0].q, ST_Q_TOTAL_LOG);
    CHECK_EQ_INT(d.v[1].q, ST_Q_TRIP_LOG);
    CHECK_NEAR(d.v[0].value, 1234.5 * NM, 0.05 * NM + 1e-6);
    CHECK_NEAR(d.v[1].value, 12.34 * NM, 0.005 * NM + 1e-6);

    CHECK_EQ_INT(dec(sog, sizeof sog, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 6.2 * KN, 0.05 * KN + 1e-9);

    CHECK_EQ_INT(dec(cog, sizeof cog, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.v[0].q, ST_Q_COURSE_OVER_GROUND_MAGNETIC);
    CHECK_NEAR(d.v[0].value, 123 * DEG, 0.5 * DEG);
    CHECK_EQ_INT(dec(h53, sizeof h53, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 234 * DEG, 0.5 * DEG);
    CHECK_EQ_INT(dec(h89, sizeof h89, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 234 * DEG, 0.5 * DEG);
    CHECK_EQ_INT(dec(h9c, sizeof h9c, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.n, 2);
    CHECK_NEAR(d.v[0].value, 234 * DEG, 0.5 * DEG);
    CHECK_NEAR(d.v[1].value, 0.0, 1e-12);
}

/* ---- 2. odd headings -------------------------------------------------- */

static void check_heading_roundtrip(uint8_t cmd, st_quantity_t q, double deg, double expect_deg)
{
    uint8_t o[18];
    st_decoded_t d;
    uint8_t n = enc1(cmd, q, deg * DEG, o);
    CHECK(n > 0);
    if (n == 0) return;
    CHECK_EQ_INT(dec(o, n, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.v[0].q, q);
    CHECK_NEAR(d.v[0].value, expect_deg * DEG, 1e-9);
}

static void test_odd_headings(void)
{
    uint8_t o[18];
    static const double degs[] = {0, 1, 89, 90, 235, 359};
    size_t i;

    CHECK_EQ_INT(enc1(0x53, ST_Q_COURSE_OVER_GROUND_MAGNETIC, 235 * DEG, o), 3);
    CHECK_EQ_INT(((o[1] >> 4) & 0x8), 0x8);
    CHECK_EQ_INT(enc1(0x89, ST_Q_HEADING_MAGNETIC, 235 * DEG, o), 5);
    CHECK_EQ_INT(((o[1] >> 4) & 0x8), 0x8);
    CHECK_EQ_INT(((o[1] >> 4) & 0x4), 0);
    CHECK_EQ_INT(enc1(0x9C, ST_Q_HEADING_MAGNETIC, 235 * DEG, o), 4);
    CHECK_EQ_INT(((o[1] >> 4) & 0x4), 0x4);
    CHECK_EQ_INT(((o[1] >> 4) & 0x8), 0);

    check_heading_roundtrip(0x53, ST_Q_COURSE_OVER_GROUND_MAGNETIC, 235, 235);
    check_heading_roundtrip(0x89, ST_Q_HEADING_MAGNETIC, 235, 235);
    check_heading_roundtrip(0x9C, ST_Q_HEADING_MAGNETIC, 235, 235);

    for (i = 0; i < ARRAY_LEN(degs); i++) {
        check_heading_roundtrip(0x53, ST_Q_COURSE_OVER_GROUND_MAGNETIC, degs[i], degs[i]);
        check_heading_roundtrip(0x89, ST_Q_HEADING_MAGNETIC, degs[i], degs[i]);
        check_heading_roundtrip(0x9C, ST_Q_HEADING_MAGNETIC, degs[i], degs[i]);
    }
    /* 359.6 rounds to 360 -> 0 */
    check_heading_roundtrip(0x53, ST_Q_COURSE_OVER_GROUND_MAGNETIC, 359.6, 0);
    check_heading_roundtrip(0x89, ST_Q_HEADING_MAGNETIC, 359.6, 0);
    check_heading_roundtrip(0x9C, ST_Q_HEADING_MAGNETIC, 359.6, 0);
    /* negative radians are normalised */
    check_heading_roundtrip(0x53, ST_Q_COURSE_OVER_GROUND_MAGNETIC, -90, 270);
}

/* ---- 3. Knauf decode cases ------------------------------------------- */

static void test_knauf_decode(void)
{
    st_decoded_t d;
    /* 9C, U = 0xC: 0*90 + 16*2 + 2 = 34, turning right */
    static const uint8_t c9c[] = {0x9C, 0xC1, 0x10, 0x00};
    /* 9C, U = 0x8: 32 + 1 = 33, turning right */
    static const uint8_t c9c8[] = {0x9C, 0x81, 0x10, 0x00};
    /* 9C, U = 0x4: 32 + 1 = 33, not turning */
    static const uint8_t c9c4[] = {0x9C, 0x41, 0x10, 0x00};
    /* 53, U&0xC = 0x4: 32 + 0 */
    static const uint8_t c53[] = {0x53, 0x40, 0x10};
    /* 53, U = 0x8: 32 + 1; U = 0xC: 32 + 1 */
    static const uint8_t c538[] = {0x53, 0x80, 0x10};
    static const uint8_t c53c[] = {0x53, 0xC0, 0x10};
    static const uint8_t rud[] = {0x9C, 0x01, 0x00, 0xFE};
    static const uint8_t awa360[] = {0x10, 0x01, 0x01, 0x68};
    static const uint8_t awa540[] = {0x10, 0x01, 0x02, 0x1C};

    CHECK_EQ_INT(dec(c9c, sizeof c9c, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 34 * DEG, 1e-9);
    CHECK(d.flags & ST_DF_TURNING_RIGHT);
    CHECK_EQ_INT(dec(c9c8, sizeof c9c8, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 33 * DEG, 1e-9);
    CHECK(d.flags & ST_DF_TURNING_RIGHT);
    CHECK_EQ_INT(dec(c9c4, sizeof c9c4, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 33 * DEG, 1e-9);
    CHECK_EQ_INT(d.flags & ST_DF_TURNING_RIGHT, 0);

    CHECK_EQ_INT(dec(c53, sizeof c53, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 32 * DEG, 1e-9);
    CHECK_EQ_INT(dec(c538, sizeof c538, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 33 * DEG, 1e-9);
    CHECK_EQ_INT(dec(c53c, sizeof c53c, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 33 * DEG, 1e-9);

    CHECK_EQ_INT(dec(rud, sizeof rud, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.v[1].q, ST_Q_RUDDER_ANGLE);
    CHECK_NEAR(d.v[1].value, -2 * DEG, 1e-9);

    CHECK_EQ_INT(dec(awa360, sizeof awa360, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, ST_PI, 1e-9);
    CHECK_EQ_INT(dec(awa540, sizeof awa540, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, -90 * DEG, 1e-9);
}

/* ---- 4. flags --------------------------------------------------------- */

static void test_flags(void)
{
    st_decoded_t d;
    static const uint8_t def[] = {0x00, 0x02, 0x04, 0x94, 0x01};
    static const uint8_t anchor[] = {0x00, 0x02, 0xC0, 0x94, 0x01};
    static const uint8_t alarms[] = {0x00, 0x02, 0x03, 0x94, 0x01};
    static const uint8_t ms[] = {0x11, 0x01, 0x8A, 0x05}; /* 10.5 m/s */
    static const uint8_t cur_inval[] = {0x26, 0x04, 0x39, 0x00, 0x39, 0x00, 0x00};
    static const uint8_t cur_mph[] = {0x26, 0x04, 0x64, 0x00, 0x00, 0x00, 0x42}; /* 1.00 mph */
    static const uint8_t cur_kn[] = {0x26, 0x04, 0x64, 0x00, 0x00, 0x00, 0x40};
    static const uint8_t t23def[] = {0x23, 0x41, 0x13, 0x41};
    static const uint8_t t23neg[] = {0x23, 0x01, 0xFB, 0x17}; /* -5 C */
    static const uint8_t lamp3[] = {0x30, 0x70, 0x0C};
    static const uint8_t lamp0[] = {0x30, 0x70, 0x00};
    static const uint8_t lamp2[] = {0x30, 0x00, 0x08};

    CHECK_EQ_INT(dec(def, sizeof def, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.n, 0);
    CHECK(d.flags & ST_DF_SENSOR_DEFECTIVE);

    CHECK_EQ_INT(dec(anchor, sizeof anchor, &d), ST_DEC_OK);
    CHECK(d.flags & ST_DF_ANCHOR_ALARM);
    CHECK(d.flags & ST_DF_DISPLAY_METRIC);
    CHECK_EQ_INT(d.n, 1);
    CHECK_NEAR(d.v[0].value, 404 / 10.0 * FT, 1e-9); /* still feet */

    CHECK_EQ_INT(dec(alarms, sizeof alarms, &d), ST_DEC_OK);
    CHECK(d.flags & ST_DF_DEEP_ALARM);
    CHECK(d.flags & ST_DF_SHALLOW_ALARM);
    CHECK_EQ_INT(d.n, 1);

    CHECK_EQ_INT(dec(ms, sizeof ms, &d), ST_DEC_OK);
    CHECK(d.flags & ST_DF_UNIT_MS);
    CHECK_NEAR(d.v[0].value, 10.5, 1e-9);

    CHECK_EQ_INT(dec(cur_inval, sizeof cur_inval, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.n, 0);
    CHECK(d.flags & ST_DF_INVALID);

    CHECK_EQ_INT(dec(cur_mph, sizeof cur_mph, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.n, 1);
    CHECK(d.flags & ST_DF_UNIT_MPH);
    CHECK_NEAR(d.v[0].value, 1.0 * MPH, 1e-9);
    CHECK_EQ_INT(dec(cur_kn, sizeof cur_kn, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.flags & ST_DF_UNIT_MPH, 0);
    CHECK_NEAR(d.v[0].value, 1.0 * KN, 1e-9);

    CHECK_EQ_INT(dec(t23def, sizeof t23def, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.n, 0);
    CHECK(d.flags & ST_DF_SENSOR_DEFECTIVE);
    CHECK_EQ_INT(dec(t23neg, sizeof t23neg, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, -5 + KELVIN, 1e-9);

    CHECK_EQ_INT(dec(lamp3, sizeof lamp3, &d), ST_DEC_OK);
    CHECK_EQ_INT(d.v[0].q, ST_Q_LAMP_LEVEL);
    CHECK_NEAR(d.v[0].value, 3, 0);
    CHECK_EQ_INT(dec(lamp0, sizeof lamp0, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 0, 0);
    CHECK_EQ_INT(dec(lamp2, sizeof lamp2, &d), ST_DEC_OK);
    CHECK_NEAR(d.v[0].value, 2, 0);
}

/* ---- 5. clamping ------------------------------------------------------ */

static void test_clamping(void)
{
    uint8_t o[18];
    vals_t s;

    CHECK_EQ_INT(enc1(0x00, ST_Q_DEPTH_BELOW_TRANSDUCER, -5.0, o), 5);
    CHECK_EQ_INT(o[3], 0);
    CHECK_EQ_INT(o[4], 0);
    CHECK_EQ_INT(enc1(0x00, ST_Q_DEPTH_BELOW_TRANSDUCER, 1e9, o), 5);
    CHECK_EQ_INT(o[3], 0xFF);
    CHECK_EQ_INT(o[4], 0xFF);

    CHECK_EQ_INT(enc1(0x20, ST_Q_SPEED_THROUGH_WATER, 7000 * KN, o), 4);
    CHECK_EQ_INT(o[2], 0xFF);
    CHECK_EQ_INT(o[3], 0xFF);

    CHECK_EQ_INT(enc1(0x11, ST_Q_WIND_SPEED_APPARENT, 200 * KN, o), 4);
    CHECK_EQ_INT(o[2], 0x7F);
    CHECK_EQ_INT(o[3], 9);

    CHECK_EQ_INT(enc1(0x10, ST_Q_WIND_ANGLE_APPARENT, -10 * DEG, o), 4);
    CHECK_EQ_INT(o[2], 0x02);
    CHECK_EQ_INT(o[3], 0xBC);

    memset(&s, 0, sizeof s);
    set(&s, ST_Q_HEADING_MAGNETIC, 10 * DEG);
    set(&s, ST_Q_RUDDER_ANGLE, 200 * DEG);
    CHECK_EQ_INT(st_codec_encode(0x9C, get_fn, &s, o), 4);
    CHECK_EQ_INT(o[3], 127);
    set(&s, ST_Q_RUDDER_ANGLE, -200 * DEG);
    CHECK_EQ_INT(st_codec_encode(0x9C, get_fn, &s, o), 4);
    CHECK_EQ_INT(o[3], 0x81); /* -127 */

    CHECK_EQ_INT(enc1(0x21, ST_Q_TRIP_LOG, 1e12, o), 5);
    CHECK_EQ_INT(o[2], 0xFF);
    CHECK_EQ_INT(o[3], 0xFF);
    CHECK_EQ_INT(o[4], 0x0F);
    CHECK_EQ_INT(enc1(0x27, ST_Q_WATER_TEMPERATURE, 0.0, o), 4); /* -273 C -> 0 */
    CHECK_EQ_INT(o[2], 0);
    CHECK_EQ_INT(o[3], 0);
    CHECK_EQ_INT(enc1(0x52, ST_Q_SPEED_OVER_GROUND, -1.0, o), 4);
    CHECK_EQ_INT(o[2], 0);
    CHECK_EQ_INT(o[3], 0);
}

/* ---- 6. missing values ------------------------------------------------ */

static void test_missing(void)
{
    uint8_t o[18];
    vals_t s;
    size_t i;
    uint8_t n;
    const st_dgram_info_t *t = st_codec_table(&n);

    memset(&s, 0, sizeof s);
    for (i = 0; i < n; i++) CHECK_EQ_INT(st_codec_encode(t[i].cmd, get_fn, &s, o), 0);

    /* Non-finite values (Signal K null, broken source) are missing too: the
     * slot stays silent instead of sending 0 (spec 5.2 rule 7). */
    {
        const double bad[] = {NAN, INFINITY, -INFINITY};
        for (size_t k = 0; k < ARRAY_LEN(bad); k++) {
            for (i = 0; i < n; i++) {
                vals_t all;
                memset(&all, 0, sizeof all);
                for (int q = 1; q < (int)ST_Q_COUNT; q++) set(&all, (st_quantity_t)q, bad[k]);
                CHECK_EQ_INT(st_codec_encode(t[i].cmd, get_fn, &all, o), 0);
            }
        }
    }

    set(&s, ST_Q_TOTAL_LOG, 1000.0);
    CHECK_EQ_INT(st_codec_encode(0x25, get_fn, &s, o), 0);
    memset(&s, 0, sizeof s);
    set(&s, ST_Q_TRIP_LOG, 1000.0);
    CHECK_EQ_INT(st_codec_encode(0x25, get_fn, &s, o), 0);

    memset(&s, 0, sizeof s);
    set(&s, ST_Q_HEADING_MAGNETIC, 90 * DEG);
    memset(o, 0xAA, sizeof o);
    CHECK_EQ_INT(st_codec_encode(0x9C, get_fn, &s, o), 4);
    CHECK_EQ_INT(o[3], 0);
    CHECK_EQ_INT(o[18 - 1], 0xAA); /* nothing written past the datagram */
    CHECK_EQ_INT(o[4], 0xAA);

    /* rudder only: heading is required */
    memset(&s, 0, sizeof s);
    set(&s, ST_Q_RUDDER_ANGLE, 0.1);
    CHECK_EQ_INT(st_codec_encode(0x9C, get_fn, &s, o), 0);
    CHECK_EQ_INT(st_codec_encode(0x30, get_fn, &s, o), 0);
}

/* ---- 7. table --------------------------------------------------------- */

static bool all_get(st_quantity_t q, double *v, void *arg)
{
    (void)arg;
    switch (q) {
    case ST_Q_DEPTH_BELOW_TRANSDUCER: *v = 5.0; return true;
    case ST_Q_WIND_ANGLE_APPARENT: *v = 0.5; return true;
    case ST_Q_WIND_SPEED_APPARENT: *v = 6.0; return true;
    case ST_Q_SPEED_THROUGH_WATER: *v = 3.0; return true;
    case ST_Q_TRIP_LOG: *v = 5000.0; return true;
    case ST_Q_TOTAL_LOG: *v = 50000.0; return true;
    case ST_Q_WATER_TEMPERATURE: *v = 290.0; return true;
    case ST_Q_SPEED_OVER_GROUND: *v = 3.0; return true;
    case ST_Q_COURSE_OVER_GROUND_MAGNETIC: *v = 1.0; return true;
    case ST_Q_HEADING_MAGNETIC: *v = 2.0; return true;
    case ST_Q_RUDDER_ANGLE: *v = 0.1; return true;
    default: return false;
    }
}

static void test_table(void)
{
    uint8_t n = 0, i;
    const st_dgram_info_t *t = st_codec_table(&n);
    double rate = 0.0;
    st_decoded_t d;
    uint8_t b[8] = {0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    CHECK_EQ_INT(n, 15);
    for (i = 0; i < n; i++) {
        uint8_t o[18];
        uint8_t len;
        CHECK(st_codec_info(t[i].cmd) == &t[i]);
        CHECK(t[i].name != NULL && t[i].name[0] != 0);
        CHECK(t[i].can_decode);
        len = st_codec_encode(t[i].cmd, all_get, NULL, o);
        if (t[i].can_encode) {
            CHECK_EQ_INT(len, t[i].len);
            CHECK_EQ_INT(o[0], t[i].cmd);
            CHECK_EQ_INT((o[1] & 0x0F) + 3, t[i].len);
            /* and it decodes with the table length */
            CHECK_EQ_INT(st_codec_decode(o, len, &d), ST_DEC_OK);
            /* one byte off either way is a length error */
            CHECK_EQ_INT(st_codec_decode(o, (uint8_t)(len + 1), &d), ST_DEC_BAD_LENGTH);
            CHECK_EQ_INT(st_codec_decode(o, (uint8_t)(len - 1), &d), ST_DEC_BAD_LENGTH);
        } else {
            CHECK_EQ_INT(len, 0);
        }
        if (t[i].default_period_ms > 0) rate += 1000.0 / (double)t[i].default_period_ms;
    }
    CHECK_NEAR(rate, 9.4, 1e-9);

    CHECK(st_codec_info(0x30) != NULL);
    CHECK(!st_codec_info(0x30)->can_encode);
    CHECK(st_codec_info(0x84) == NULL);
    CHECK_EQ_INT(st_codec_decode(b, 7, &d), ST_DEC_UNKNOWN);
    CHECK_EQ_INT(st_codec_encode(0x84, all_get, NULL, b), 0);

    CHECK(strcmp(st_quantity_name(ST_Q_NONE), "none") == 0);
    CHECK(strcmp(st_quantity_name(ST_Q_DEPTH_BELOW_TRANSDUCER), "depth_below_transducer") == 0);
    CHECK(strcmp(st_quantity_name(ST_Q_LAMP_LEVEL), "lamp_level") == 0);
    CHECK(strcmp(st_quantity_name(ST_Q_COUNT), "?") == 0);
    CHECK(strcmp(st_quantity_name((st_quantity_t)99), "?") == 0);
}

/* ---- 8. round trip sweeps --------------------------------------------- */

typedef struct {
    uint8_t cmd;
    st_quantity_t q;
    double lo, hi;
    double half_lsb; /* SI units */
    bool angular;    /* compare modulo 2pi */
} sweep_t;

static double angdiff(double a, double b)
{
    double d = fmod(a - b, 2 * ST_PI);
    if (d > ST_PI) d -= 2 * ST_PI;
    if (d < -ST_PI) d += 2 * ST_PI;
    return fabs(d);
}

static void test_roundtrip_sweep(void)
{
    static const sweep_t cases[] = {
        {0x00, ST_Q_DEPTH_BELOW_TRANSDUCER, 0, 1900, 0.05 * FT, false},
        {0x10, ST_Q_WIND_ANGLE_APPARENT, -3.1, 3.1, 0.25 * DEG, true},
        {0x11, ST_Q_WIND_SPEED_APPARENT, 0, 60, 0.05 * KN, false},
        {0x20, ST_Q_SPEED_THROUGH_WATER, 0, 100, 0.05 * KN, false},
        {0x21, ST_Q_TRIP_LOG, 0, 10000 * NM, 0.005 * NM, false},
        {0x22, ST_Q_TOTAL_LOG, 0, 6500 * NM, 0.05 * NM, false},
        {0x23, ST_Q_WATER_TEMPERATURE, -20 + KELVIN, 40 + KELVIN, 0.5, false},
        {0x25, ST_Q_TOTAL_LOG, 0, 100000 * NM, 0.05 * NM, false},
        {0x25, ST_Q_TRIP_LOG, 0, 10000 * NM, 0.005 * NM, false},
        {0x26, ST_Q_SPEED_THROUGH_WATER, 0, 50, 0.005 * KN, false},
        {0x27, ST_Q_WATER_TEMPERATURE, -5 + KELVIN, 50 + KELVIN, 0.05, false},
        {0x52, ST_Q_SPEED_OVER_GROUND, 0, 100, 0.05 * KN, false},
        {0x53, ST_Q_COURSE_OVER_GROUND_MAGNETIC, 0, 2 * ST_PI, 0.5 * DEG, true},
        {0x89, ST_Q_HEADING_MAGNETIC, 0, 2 * ST_PI, 0.5 * DEG, true},
        {0x9C, ST_Q_HEADING_MAGNETIC, 0, 2 * ST_PI, 0.5 * DEG, true},
        {0x9C, ST_Q_RUDDER_ANGLE, -120 * DEG, 120 * DEG, 0.5 * DEG, false},
    };
    size_t c;
    int k;

    for (c = 0; c < ARRAY_LEN(cases); c++) {
        const sweep_t *w = &cases[c];
        int bad = 0;
        for (k = 0; k <= 1000; k++) {
            double x = w->lo + (w->hi - w->lo) * (double)k / 1000.0;
            vals_t s;
            uint8_t o[18];
            uint8_t n;
            st_decoded_t d;
            int i, found = -1;
            double err;

            /* defaults for the companions of two-value datagrams */
            memset(&s, 0, sizeof s);
            set(&s, ST_Q_TOTAL_LOG, 100.0 * NM);
            set(&s, ST_Q_TRIP_LOG, 10.0 * NM);
            set(&s, ST_Q_HEADING_MAGNETIC, 1.0);
            set(&s, ST_Q_RUDDER_ANGLE, 0.0);
            set(&s, w->q, x);

            n = st_codec_encode(w->cmd, get_fn, &s, o);
            if (n == 0 || st_codec_decode(o, n, &d) != ST_DEC_OK) { bad++; continue; }
            for (i = 0; i < d.n; i++) if (d.v[i].q == w->q) found = i;
            if (found < 0) { bad++; continue; }
            err = w->angular ? angdiff(d.v[found].value, x) : fabs(d.v[found].value - x);
            if (err > w->half_lsb + 1e-9 * (1.0 + fabs(x))) {
                if (bad < 3)
                    printf("  sweep cmd %02X q %d x=%.9g got %.9g\n", w->cmd, (int)w->q, x,
                           d.v[found].value);
                bad++;
            }
        }
        CHECK_EQ_INT(bad, 0);
    }
}

int main(void)
{
    RUN(test_static_boat_vectors);
    RUN(test_odd_headings);
    RUN(test_knauf_decode);
    RUN(test_flags);
    RUN(test_clamping);
    RUN(test_missing);
    RUN(test_table);
    RUN(test_roundtrip_sweep);
    return test_report();
}
