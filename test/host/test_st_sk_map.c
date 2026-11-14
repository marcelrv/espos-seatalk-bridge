/* Host unit tests for the Signal K adapter's pure part, and a check that
 * config/seatalk.json agrees with st_codec_table(). */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_sk_map.h"
#include "test_util.h"

#define PI 3.14159265358979323846

#define CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                         \
        const double va_ = (double)(a);                                          \
        const double vb_ = (double)(b);                                          \
        if (!(fabs(va_ - vb_) <= (double)(eps))) {                               \
            printf("%s:%d: CHECK_NEAR failed: %s ~ %s\n    expected %.9g, actual %.9g\n", \
                   __FILE__, __LINE__, #a, #b, vb_, va_);                        \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static void test_path_table(void)
{
    CHECK(strcmp(st_sk_path_of(ST_Q_DEPTH_BELOW_TRANSDUCER), "environment.depth.belowTransducer") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_WIND_ANGLE_APPARENT), "environment.wind.angleApparent") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_WIND_SPEED_APPARENT), "environment.wind.speedApparent") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_SPEED_THROUGH_WATER), "navigation.speedThroughWater") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_TRIP_LOG), "navigation.trip.log") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_TOTAL_LOG), "navigation.log") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_WATER_TEMPERATURE), "environment.water.temperature") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_SPEED_OVER_GROUND), "navigation.speedOverGround") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_COURSE_OVER_GROUND_MAGNETIC), "navigation.courseOverGroundMagnetic") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_HEADING_MAGNETIC), "navigation.headingMagnetic") == 0);
    CHECK(strcmp(st_sk_path_of(ST_Q_RUDDER_ANGLE), "steering.rudderAngle") == 0);
    CHECK(st_sk_path_of(ST_Q_LAMP_LEVEL) == NULL);
    CHECK(st_sk_path_of(ST_Q_NONE) == NULL);

    /* Every real quantity except the lamp has exactly one row. */
    size_t n = 0;
    const st_sk_qpath_t *t = st_sk_qpaths(&n);
    CHECK_EQ_INT(n, ST_Q_COUNT - 2);
    for (size_t i = 0; i < n; i++) {
        CHECK(t[i].src != NULL && strlen(t[i].src) + 4 <= 15); /* "src_<short>" fits an NVS key */
    }
}

static void test_subs(void)
{
    size_t n = 0;
    const st_sk_sub_t *s = st_sk_subs(&n);
    CHECK_EQ_INT(n, 14);
    int direct = 0, hdg = 0, cog = 0, var = 0;
    for (size_t i = 0; i < n; i++) {
        switch (s[i].role) {
        case ST_SK_ROLE_DIRECT:
            direct++;
            CHECK(strcmp(s[i].path, st_sk_path_of(s[i].q)) == 0);
            break;
        case ST_SK_ROLE_TRUE_HDG:
            hdg++;
            CHECK_EQ_INT(s[i].q, ST_Q_HEADING_MAGNETIC);
            CHECK(strcmp(s[i].path, "navigation.headingTrue") == 0);
            break;
        case ST_SK_ROLE_TRUE_COG:
            cog++;
            CHECK_EQ_INT(s[i].q, ST_Q_COURSE_OVER_GROUND_MAGNETIC);
            CHECK(strcmp(s[i].path, "navigation.courseOverGroundTrue") == 0);
            break;
        case ST_SK_ROLE_VARIATION:
            var++;
            CHECK(strcmp(s[i].path, "navigation.magneticVariation") == 0);
            break;
        }
        const bool wind = s[i].q == ST_Q_WIND_ANGLE_APPARENT || s[i].q == ST_Q_WIND_SPEED_APPARENT;
        CHECK_EQ_INT(s[i].period_ms, wind ? 500 : 1000);
    }
    CHECK_EQ_INT(direct, 11);
    CHECK_EQ_INT(hdg, 1);
    CHECK_EQ_INT(cog, 1);
    CHECK_EQ_INT(var, 1);
}

static void test_own_source(void)
{
    CHECK(st_sk_is_own_source("espos-f768", "espos-f768"));
    CHECK(st_sk_is_own_source("espos-f768.XX", "espos-f768"));
    CHECK(st_sk_is_own_source("espos-f768.", "espos-f768"));
    CHECK(!st_sk_is_own_source("espos-f7680", "espos-f768"));
    CHECK(!st_sk_is_own_source("espos-f76", "espos-f768"));
    CHECK(!st_sk_is_own_source("espos-f768x.XX", "espos-f768"));
    CHECK(!st_sk_is_own_source("can0.115", "espos-f768"));
    CHECK(!st_sk_is_own_source("", "espos-f768"));
    CHECK(!st_sk_is_own_source(NULL, "espos-f768")); /* NULL source is foreign */
    CHECK(!st_sk_is_own_source("espos-f768", NULL));
    CHECK(!st_sk_is_own_source("espos-f768", ""));
}

static void test_wrap(void)
{
    CHECK_NEAR(st_sk_wrap_2pi(0.0), 0.0, 1e-12);
    CHECK_NEAR(st_sk_wrap_2pi(1.0), 1.0, 1e-12);
    CHECK_NEAR(st_sk_wrap_2pi(-0.5), 2 * PI - 0.5, 1e-12);
    CHECK_NEAR(st_sk_wrap_2pi(2 * PI + 0.25), 0.25, 1e-12);
    CHECK_NEAR(st_sk_wrap_2pi(-2 * PI - 0.25), 2 * PI - 0.25, 1e-12);
    CHECK_NEAR(st_sk_wrap_2pi(2 * PI), 0.0, 1e-12);
    for (double x = -20; x < 20; x += 0.37) {
        const double w = st_sk_wrap_2pi(x);
        CHECK(w >= 0.0 && w < 2 * PI);
    }
    /* A tiny negative value must not round up to exactly 2pi. */
    CHECK(st_sk_wrap_2pi(-1e-18) < 2 * PI);
}

static void test_parse_value(void)
{
    double v = -1;
    CHECK_EQ_INT(st_sk_parse_value("1.5", &v), ST_SK_VAL_NUMBER);
    CHECK_NEAR(v, 1.5, 1e-12);
    CHECK_EQ_INT(st_sk_parse_value("-0.25", &v), ST_SK_VAL_NUMBER);
    CHECK_NEAR(v, -0.25, 1e-12);
    CHECK_EQ_INT(st_sk_parse_value("3", &v), ST_SK_VAL_NUMBER);
    CHECK_NEAR(v, 3.0, 1e-12);
    CHECK_EQ_INT(st_sk_parse_value("1e2", &v), ST_SK_VAL_NUMBER);
    CHECK_NEAR(v, 100.0, 1e-9);
    CHECK_EQ_INT(st_sk_parse_value(" 7 ", &v), ST_SK_VAL_NUMBER);
    CHECK_NEAR(v, 7.0, 1e-12);
    CHECK_EQ_INT(st_sk_parse_value("null", &v), ST_SK_VAL_NULL);
    CHECK_EQ_INT(st_sk_parse_value("null ", &v), ST_SK_VAL_NULL);

    CHECK_EQ_INT(st_sk_parse_value("1.5x", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("1.5 2", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("-", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("\"1.5\"", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("{\"value\":1}", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("[1]", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("true", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("nullx", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("NaN", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("Infinity", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("-inf", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("0x10", &v), ST_SK_VAL_BAD);
    CHECK_EQ_INT(st_sk_parse_value("1e999", &v), ST_SK_VAL_BAD); /* overflows to inf */
    CHECK_EQ_INT(st_sk_parse_value(NULL, &v), ST_SK_VAL_BAD);
}

static void test_derive_mag(void)
{
    double m = -1;
    const int64_t now = 1000000;
    const double var = 0.05;

    /* Normal case: no direct value ever, variation fresh. */
    CHECK(st_sk_derive_mag(true, now, 1.0, ST_SK_NEVER, var, now - 1000, 5000, &m));
    CHECK_NEAR(m, 0.95, 1e-12);

    /* Wraps below zero and above 2pi. */
    CHECK(st_sk_derive_mag(true, now, 0.02, ST_SK_NEVER, var, now, 5000, &m));
    CHECK_NEAR(m, 2 * PI - 0.03, 1e-12);
    CHECK(st_sk_derive_mag(true, now, 2 * PI - 0.01, ST_SK_NEVER, -var, now, 5000, &m));
    CHECK_NEAR(m, 0.04, 1e-12);

    /* Disabled. */
    CHECK(!st_sk_derive_mag(false, now, 1.0, ST_SK_NEVER, var, now, 5000, &m));

    /* Direct magnetic fresh (<= stale_ms) always wins; stale it does not. */
    CHECK(!st_sk_derive_mag(true, now, 1.0, now - 5000, var, now, 5000, &m));
    CHECK(!st_sk_derive_mag(true, now, 1.0, now - 100, var, now, 5000, &m));
    CHECK(st_sk_derive_mag(true, now, 1.0, now - 5001, var, now, 5000, &m));

    /* Variation unknown, too old (> 10 min), or just inside. */
    CHECK(!st_sk_derive_mag(true, now, 1.0, ST_SK_NEVER, var, ST_SK_NEVER, 5000, &m));
    CHECK(!st_sk_derive_mag(true, now, 1.0, ST_SK_NEVER, var, now - ST_SK_VARIATION_MAX_AGE_MS - 1, 5000, &m));
    CHECK(st_sk_derive_mag(true, now, 1.0, ST_SK_NEVER, var, now - ST_SK_VARIATION_MAX_AGE_MS, 5000, &m));

    /* Non-finite inputs. */
    CHECK(!st_sk_derive_mag(true, now, NAN, ST_SK_NEVER, var, now, 5000, &m));
    CHECK(!st_sk_derive_mag(true, now, 1.0, ST_SK_NEVER, NAN, now, 5000, &m));
}

static void test_subst(void)
{
    st_sk_subst_t m = ST_SK_SUBST_REPLACE;
    CHECK(st_sk_subst_from_name("off", &m) && m == ST_SK_SUBST_OFF);
    CHECK(st_sk_subst_from_name("fallback", &m) && m == ST_SK_SUBST_FALLBACK);
    CHECK(st_sk_subst_from_name("replace", &m) && m == ST_SK_SUBST_REPLACE);
    CHECK(!st_sk_subst_from_name("Replace", &m));
    CHECK(!st_sk_subst_from_name("", &m));
    CHECK(!st_sk_subst_from_name(NULL, &m));

    const int64_t now = 1000000;
    CHECK(!st_sk_use_subst(ST_SK_SUBST_OFF, now, ST_SK_NEVER, 5000));
    CHECK(st_sk_use_subst(ST_SK_SUBST_REPLACE, now, now, 5000));
    /* fallback: only while the native value is missing or stale */
    CHECK(st_sk_use_subst(ST_SK_SUBST_FALLBACK, now, ST_SK_NEVER, 5000));
    CHECK(!st_sk_use_subst(ST_SK_SUBST_FALLBACK, now, now - 5000, 5000));
    CHECK(st_sk_use_subst(ST_SK_SUBST_FALLBACK, now, now - 5001, 5000));
}

static void test_defective(void)
{
    CHECK(strcmp(st_sk_defective_path(0x00), "environment.depth.belowTransducer") == 0);
    CHECK(strcmp(st_sk_defective_path(0x23), "environment.water.temperature") == 0);
    CHECK(st_sk_defective_path(0x27) == NULL);
    CHECK(st_sk_defective_path(0x10) == NULL);
}

/* ---- descriptor vs codec table ---- */

static char *s_json;

static bool load_json(void)
{
    FILE *f = fopen(ST_SK_DESCRIPTOR, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_json = (char *)malloc((size_t)n + 1);
    if (!s_json || fread(s_json, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return false;
    }
    s_json[n] = '\0';
    fclose(f);
    return true;
}

/* The descriptor has one key object per line. */
static const char *find_key_line(const char *name)
{
    char pat[48];
    snprintf(pat, sizeof pat, "{\"name\": \"%s\"", name);
    return strstr(s_json, pat);
}

static bool line_has(const char *line, const char *text)
{
    const char *eol = strchr(line, '\n');
    const char *p = strstr(line, text);
    return p && (!eol || p < eol);
}

static long line_number_after(const char *line, const char *label)
{
    const char *eol = strchr(line, '\n');
    const char *p = strstr(line, label);
    if (!p || (eol && p > eol)) {
        return -1;
    }
    return strtol(p + strlen(label), NULL, 10);
}

static void test_descriptor_matches_codec(void)
{
    CHECK(load_json());
    if (!s_json) {
        return;
    }
    uint8_t n = 0;
    const st_dgram_info_t *tab = st_codec_table(&n);
    CHECK_EQ_INT(n, 15);
    for (uint8_t i = 0; i < n; i++) {
        char key[16], grp[48];
        snprintf(grp, sizeof grp, "\"group\": \"%02X %s\"", tab[i].cmd, tab[i].name);
        snprintf(key, sizeof key, "m_%02x", tab[i].cmd);
        const char *m = find_key_line(key);
        CHECK(m != NULL);
        if (m) {
            CHECK(line_has(m, "\"enum\": [\"off\", \"to_sk\", \"to_bus\", \"both\", \"auto\"]"));
            CHECK(line_has(m, "\"default\": \"auto\""));
            CHECK(line_has(m, grp));
        }
        snprintf(key, sizeof key, "p_%02x", tab[i].cmd);
        const char *p = find_key_line(key);
        if (tab[i].can_encode) {
            CHECK(p != NULL);
            if (p) {
                CHECK_EQ_INT(line_number_after(p, "\"default\": "), tab[i].default_period_ms);
                CHECK_EQ_INT(line_number_after(p, "\"min\": "), 0);
                CHECK_EQ_INT(line_number_after(p, "\"max\": "), 60000);
                CHECK(line_has(p, grp));
            }
        } else {
            CHECK(p == NULL); /* decode-only: mode only */
        }
    }
    /* Quantity source filters and the general keys. */
    size_t nq = 0;
    const st_sk_qpath_t *qp = st_sk_qpaths(&nq);
    for (size_t i = 0; i < nq; i++) {
        char key[24];
        snprintf(key, sizeof key, "src_%s", qp[i].src);
        const char *k = find_key_line(key);
        CHECK(k != NULL);
        if (k) {
            CHECK(line_has(k, "\"maxLength\": 64"));
            CHECK(line_has(k, "\"group\": \"Sources\""));
            CHECK(line_has(k, "\"default\": \"\""));
        }
    }
    const char *k = find_key_line("idle_us");
    CHECK(k && line_number_after(k, "\"default\": ") == 30000 && line_number_after(k, "\"min\": ") == 20000 &&
          line_number_after(k, "\"max\": ") == 100000 && line_has(k, "\"displayMultiplier\": 0.001"));
    k = find_key_line("jitter_us");
    CHECK(k && line_number_after(k, "\"default\": ") == 4000 && line_number_after(k, "\"max\": ") == 20000);
    k = find_key_line("stale_ms");
    CHECK(k && line_number_after(k, "\"default\": ") == 5000 && line_number_after(k, "\"min\": ") == 1000 &&
          line_number_after(k, "\"max\": ") == 60000);
    k = find_key_line("mag_fallback");
    CHECK(k && line_has(k, "\"type\": \"bool\"") && line_has(k, "\"default\": true"));

    static const char *const subst[] = {"stw_from_sog", "hdg_from_cog"};
    for (int i = 0; i < 2; i++) {
        k = find_key_line(subst[i]);
        CHECK(k && line_has(k, "\"type\": \"string\"") && line_has(k, "\"default\": \"off\"") &&
              line_has(k, "\"enum\": [\"off\", \"fallback\", \"replace\"]"));
    }

    /* Key count: 4 general + 2 substitutes + 15 modes + 14 periods + 11 sources. */
    int keys = 0;
    for (const char *p = s_json; (p = strstr(p, "{\"name\": \"")) != NULL; p++) {
        keys++;
    }
    CHECK_EQ_INT(keys, 4 + 2 + 15 + 14 + 11);
    free(s_json);
}

int main(void)
{
    RUN(test_path_table);
    RUN(test_subs);
    RUN(test_own_source);
    RUN(test_wrap);
    RUN(test_parse_value);
    RUN(test_derive_mag);
    RUN(test_subst);
    RUN(test_defective);
    RUN(test_descriptor_matches_codec);
    return test_report();
}
