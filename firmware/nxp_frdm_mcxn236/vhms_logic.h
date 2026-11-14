/*
 * VHMS - hardware independent logic for the FRDM-MCXN236 node
 *
 *   - NMEA RMC parser (GPS position + speed)
 *   - CAN payload packers for 0x105 / 0x10A / 0x10B
 *
 * Header-only (static inline) so no change to the project's CMake file is
 * needed. It has no SDK dependency and is unit tested on a PC (test_logic.c).
 */

#ifndef VHMS_LOGIC_H
#define VHMS_LOGIC_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NMEA_MAX_LEN 100U

typedef struct
{
    char    buf[NMEA_MAX_LEN];
    uint8_t len;
    bool    overflow;
    bool    inSentence;
} nmea_parser_t;

typedef struct
{
    bool     valid;          /* receiver reports a fix (RMC status 'A')   */
    int32_t  lat_e6;         /* degrees x 1e6, south is negative          */
    int32_t  lon_e6;         /* degrees x 1e6, west is negative           */
    uint16_t speed_kmh_x10;  /* km/h x 10                                 */
} gps_fix_t;

static inline void nmea_init(nmea_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

static inline int vhms_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "ddmm.mmmm" (degDigits = 2) or "dddmm.mmmm" (degDigits = 3) -> degrees x 1e6 */
static inline bool vhms_parse_coord(const char *s, uint8_t degDigits, int32_t *out_e6)
{
    int32_t deg = 0;
    int32_t minE6;
    int32_t scale = 100000;

    for (uint8_t i = 0; i < degDigits; i++)
    {
        if (s[i] < '0' || s[i] > '9') return false;
        deg = deg * 10 + (s[i] - '0');
    }
    s += degDigits;

    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
    minE6 = ((s[0] - '0') * 10 + (s[1] - '0')) * 1000000;
    s += 2;

    if (*s == '.')
    {
        s++;
        while (*s >= '0' && *s <= '9')
        {
            if (scale > 0)
            {
                minE6 += (*s - '0') * scale;
                scale /= 10;
            }
            s++;
        }
    }

    if (*s != '\0') return false;
    if (minE6 >= 60000000) return false;

    *out_e6 = deg * 1000000 + (minE6 + 30) / 60;
    return true;
}

/* decimal number "12.345" -> 12345 (x1000). An empty string means 0. */
static inline bool vhms_parse_x1000(const char *s, int32_t *out)
{
    int32_t ip = 0;
    int32_t frac = 0;
    int32_t scale = 100;
    bool any = false;

    if (*s == '\0')
    {
        *out = 0;
        return true;
    }

    while (*s >= '0' && *s <= '9')
    {
        ip = ip * 10 + (*s - '0');
        if (ip > 99999) return false;
        any = true;
        s++;
    }

    if (*s == '.')
    {
        s++;
        while (*s >= '0' && *s <= '9')
        {
            if (scale > 0)
            {
                frac += (*s - '0') * scale;
                scale /= 10;
            }
            any = true;
            s++;
        }
    }

    if (!any || *s != '\0') return false;

    *out = ip * 1000 + frac;
    return true;
}

/*
 * Parse one sentence (text after '$', up to but excluding the line end,
 * NUL terminated, modified in place). Returns true when it was a valid RMC
 * sentence; *fix is updated (fix->valid false when the receiver has no fix).
 */
static inline bool vhms_nmea_parse(char *s, uint8_t len, gps_fix_t *fix)
{
    char *star = strchr(s, '*');
    uint8_t cs = 0;
    int h1, h2;
    char *f[13];
    uint8_t n = 0;
    char *p;
    int32_t lat, lon, knots;

    if (star == NULL || (s + len) - star < 3) return false;

    for (char *q = s; q < star; q++) cs ^= (uint8_t)*q;

    h1 = vhms_hexval(star[1]);
    h2 = vhms_hexval(star[2]);
    if (h1 < 0 || h2 < 0 || cs != (uint8_t)((h1 << 4) | h2)) return false;

    if (len < 6 || strncmp(s + 2, "RMC,", 4) != 0) return false;   /* GPRMC, GNRMC ... */

    *star = '\0';

    p = s;
    f[n++] = p;
    while (*p && n < 13)
    {
        if (*p == ',')
        {
            *p = '\0';
            f[n++] = p + 1;
        }
        p++;
    }
    if (n < 8) return false;

    if (f[2][0] != 'A')
    {
        fix->valid = false;
        return true;
    }

    if ((f[4][0] != 'N' && f[4][0] != 'S') || (f[6][0] != 'E' && f[6][0] != 'W')) return false;
    if (!vhms_parse_coord(f[3], 2, &lat) || lat > 90000000) return false;
    if (!vhms_parse_coord(f[5], 3, &lon) || lon > 180000000) return false;
    if (!vhms_parse_x1000(f[7], &knots)) return false;

    {
        int64_t kmhx10 = ((int64_t)knots * 1852 + 50000) / 100000;   /* knots x1000 -> km/h x10 */
        if (kmhx10 > 65535) kmhx10 = 65535;
        fix->speed_kmh_x10 = (uint16_t)kmhx10;
    }

    fix->lat_e6 = (f[4][0] == 'S') ? -lat : lat;
    fix->lon_e6 = (f[6][0] == 'W') ? -lon : lon;
    fix->valid  = true;
    return true;
}

/* Feed one received character. True when a complete RMC sentence was parsed. */
static inline bool nmea_feed(nmea_parser_t *p, char c, gps_fix_t *fix)
{
    if (c == '$')
    {
        p->len = 0;
        p->overflow = false;
        p->inSentence = true;
        return false;
    }

    if (!p->inSentence) return false;

    if (c == '\r' || c == '\n')
    {
        p->inSentence = false;
        if (p->overflow || p->len == 0) return false;
        p->buf[p->len] = '\0';
        return vhms_nmea_parse(p->buf, p->len, fix);
    }

    if (p->len < NMEA_MAX_LEN - 1U) p->buf[p->len++] = c;
    else                            p->overflow = true;

    return false;
}

/* ---- CAN payload packers (big-endian, same formats as the server decoder) ---- */

/* 0x105: speed, uint16, km/h x10 */
static inline void pack_speed(uint8_t out[2], uint16_t speed_kmh_x10)
{
    out[0] = (uint8_t)(speed_kmh_x10 >> 8);
    out[1] = (uint8_t)(speed_kmh_x10 & 0xFFU);
}

static inline void vhms_put_i16(uint8_t *out, int16_t v)
{
    out[0] = (uint8_t)(((uint16_t)v) >> 8);
    out[1] = (uint8_t)(((uint16_t)v) & 0xFFU);
}

/* 0x10A: 3 x int16, milli-g (X, Y, Z) */
static inline void pack_accel(uint8_t out[6], int16_t x_mg, int16_t y_mg, int16_t z_mg)
{
    vhms_put_i16(&out[0], x_mg);
    vhms_put_i16(&out[2], y_mg);
    vhms_put_i16(&out[4], z_mg);
}

/* 0x10B: 2 x int32, degrees x 1e6 (latitude, longitude) */
static inline void pack_position(uint8_t out[8], int32_t lat_e6, int32_t lon_e6)
{
    uint32_t a = (uint32_t)lat_e6;
    uint32_t b = (uint32_t)lon_e6;

    out[0] = (uint8_t)(a >> 24); out[1] = (uint8_t)(a >> 16);
    out[2] = (uint8_t)(a >> 8);  out[3] = (uint8_t)a;
    out[4] = (uint8_t)(b >> 24); out[5] = (uint8_t)(b >> 16);
    out[6] = (uint8_t)(b >> 8);  out[7] = (uint8_t)b;
}

/* FXLS8974 +-2 g range: 1024 counts per g, 12-bit value sign extended to int16 */
static inline int16_t accel_raw_to_mg(int16_t raw)
{
    return (int16_t)(((int32_t)raw * 1000) / 1024);
}

#endif /* VHMS_LOGIC_H */
