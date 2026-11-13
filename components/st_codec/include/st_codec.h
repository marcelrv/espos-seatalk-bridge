/*
 * SeaTalk 1 codec (spec section 6): datagram bytes <-> neutral values.
 *
 * Pure C, no ESP-IDF, no Signal K: unit-tested on a PC (test/host).
 * Values are SI with Signal K's meaning and sign conventions:
 *   metres, m/s, radians, kelvin. Angles relative to the bow are -pi..pi
 *   (negative = port); headings/courses are 0..2pi.
 * Adapters map quantities to Signal K paths or NMEA 0183 sentences.
 *
 * Formulas: Thomas Knauf, "SeaTalk Technical Reference" (seatalk2.htm).
 * Encoder byte layouts follow the st50-emulator, whose output the ST50
 * Multi displays correctly, with one deliberate difference: 89 uses the
 * odd-degree flag 0x8 (Knauf: (U & 0xC) / 8), not the emulator's 0x4.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Neutral quantities. Never renumber: the value is used in config. */
typedef enum {
    ST_Q_NONE = 0,
    ST_Q_DEPTH_BELOW_TRANSDUCER = 1, /* m      environment.depth.belowTransducer */
    ST_Q_WIND_ANGLE_APPARENT = 2,    /* rad    environment.wind.angleApparent (-pi..pi) */
    ST_Q_WIND_SPEED_APPARENT = 3,    /* m/s    environment.wind.speedApparent */
    ST_Q_SPEED_THROUGH_WATER = 4,    /* m/s    navigation.speedThroughWater */
    ST_Q_TRIP_LOG = 5,               /* m      navigation.trip.log */
    ST_Q_TOTAL_LOG = 6,              /* m      navigation.log */
    ST_Q_WATER_TEMPERATURE = 7,      /* K      environment.water.temperature */
    ST_Q_SPEED_OVER_GROUND = 8,      /* m/s    navigation.speedOverGround */
    ST_Q_COURSE_OVER_GROUND_MAGNETIC = 9, /* rad navigation.courseOverGroundMagnetic (0..2pi) */
    ST_Q_HEADING_MAGNETIC = 10,      /* rad    navigation.headingMagnetic (0..2pi) */
    ST_Q_RUDDER_ANGLE = 11,          /* rad    steering.rudderAngle (+ = starboard) */
    ST_Q_LAMP_LEVEL = 12,            /* 0..3   internal: display backlight level (not SI) */
    ST_Q_COUNT
} st_quantity_t;

typedef struct {
    st_quantity_t q;
    double value;
} st_value_t;

/* Flags reported by the decoder (st_decoded_t.flags). */
enum {
    ST_DF_SENSOR_DEFECTIVE = 0x0001, /* 00 Z&4, 23 Z&4: no value emitted */
    ST_DF_ANCHOR_ALARM = 0x0002,     /* 00 Y&8 */
    ST_DF_DEEP_ALARM = 0x0004,       /* 00 Z&2 */
    ST_DF_SHALLOW_ALARM = 0x0008,    /* 00 Z&1 */
    ST_DF_DISPLAY_METRIC = 0x0010,   /* 00 Y&4: display units only, value is still feet */
    ST_DF_UNIT_MS = 0x0020,          /* 11 XX&0x80: number is m/s, not knots */
    ST_DF_UNIT_MPH = 0x0040,         /* 26 E&2: number is mph, not knots */
    ST_DF_INVALID = 0x0080,          /* 26 D&4 == 0: current speed not valid */
    ST_DF_TURNING_RIGHT = 0x0100,    /* 9C U&8 */
};

typedef enum {
    ST_DEC_OK = 0,       /* n values in v[] (n may be 0 when flags say why) */
    ST_DEC_UNKNOWN,      /* not a v1 datagram: raw callback only */
    ST_DEC_BAD_LENGTH,   /* known command, wrong length: drop and count */
} st_dec_status_t;

#define ST_DEC_MAX_VALUES 2

typedef struct {
    uint8_t n;
    st_value_t v[ST_DEC_MAX_VALUES];
    uint16_t flags;
} st_decoded_t;

/* Decode one complete, valid datagram (from st_link, ST_LINK_DATAGRAM only). */
st_dec_status_t st_codec_decode(const uint8_t *b, uint8_t len, st_decoded_t *out);

/* Value source for the encoder: returns false when the quantity has no
 * (fresh) value. The scheduler decides freshness; the codec only asks. */
typedef bool (*st_value_get_fn)(st_quantity_t q, double *value, void *arg);

/* Encode datagram 'cmd' into out (>= 18 bytes). Returns the length, or 0
 * when cmd can't be encoded or a required value is missing or not finite
 * (the slot then stays silent; never 0 on the display). Optional inputs: 9C rudder (0 when missing). 25 needs both
 * total and trip. Out-of-range values are clamped to what the field holds. */
uint8_t st_codec_encode(uint8_t cmd, st_value_get_fn get, void *arg, uint8_t *out);

/* Static description of a v1 datagram type. */
typedef struct {
    uint8_t cmd;
    uint8_t len;               /* total length in bytes */
    const char *name;          /* "depth", "wind_angle", ... (config key) */
    bool can_decode;
    bool can_encode;
    st_quantity_t q[ST_DEC_MAX_VALUES]; /* quantities it carries (ST_Q_NONE = unused) */
    uint32_t default_period_ms; /* spec 5.2 rule 6; 0 = not sent by default */
} st_dgram_info_t;

/* Info for cmd, or NULL when it isn't a v1 datagram. */
const st_dgram_info_t *st_codec_info(uint8_t cmd);

/* All v1 datagram types, for iterating (config UI, scheduler setup). */
const st_dgram_info_t *st_codec_table(uint8_t *count);

/* Short names for logs and adapters ("depth_below_transducer", ...). */
const char *st_quantity_name(st_quantity_t q);

#ifdef __cplusplus
}
#endif
