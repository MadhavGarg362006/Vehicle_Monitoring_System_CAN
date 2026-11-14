/* Host test for vhms_logic.h:  gcc -Wall -Wextra -o test_logic test_logic.c && ./test_logic */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vhms_logic.h"

static bool feed_str(nmea_parser_t *p, const char *s, gps_fix_t *fix)
{
    bool got = false;
    for (; *s; s++)
    {
        if (nmea_feed(p, *s, fix)) got = true;
    }
    return got;
}

int main(void)
{
    nmea_parser_t p;
    gps_fix_t fix;
    uint8_t d[8];

    nmea_init(&p);
    memset(&fix, 0, sizeof(fix));

    /* classic textbook sentence: 48 07.038 N, 11 31.000 E, 22.4 knots */
    assert(feed_str(&p, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n", &fix));
    assert(fix.valid);
    assert(fix.lat_e6 == 48117300);
    assert(fix.lon_e6 == 11516667);
    assert(fix.speed_kmh_x10 == 415);          /* 22.4 kn = 41.5 km/h */

    /* southern + western hemisphere, GN talker, leading noise, no checksum issue */
    assert(feed_str(&p, "garbage\r\n$GNRMC,010203.00,A,3352.1292,S,15112.5578,W,0.50,12.3,141126,,,A*4D\r\n", &fix));
    assert(fix.valid);
    assert(fix.lat_e6 == -33868820);
    assert(fix.lon_e6 == -151209297);
    assert(fix.speed_kmh_x10 == 9);

    /* no fix: valid becomes false, position fields are not trusted */
    assert(feed_str(&p, "$GPRMC,123519,V,,,,,,,230394,,N*7D\r\n", &fix));
    assert(!fix.valid);

    /* bad checksum is rejected and leaves the previous state alone */
    fix.valid = true;
    assert(!feed_str(&p, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6B\r\n", &fix));
    assert(fix.valid);

    /* other sentence types are ignored */
    assert(!feed_str(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n", &fix));

    /* too long line does not overflow */
    {
        char longline[300];
        memset(longline, 'A', sizeof(longline) - 1);
        longline[sizeof(longline) - 1] = '\0';
        assert(!feed_str(&p, "$", &fix));
        assert(!feed_str(&p, longline, &fix));
        assert(!feed_str(&p, "\r\n", &fix));
    }

    /* the parser recovers and still works afterwards */
    assert(feed_str(&p, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\n", &fix));
    assert(fix.valid && fix.lat_e6 == 48117300);

    /* packers: same values the server decoder test uses */
    pack_speed(d, 415);
    assert(d[0] == 0x01 && d[1] == 0x9F);

    pack_accel(d, 500, -250, 1000);
    assert(d[0] == 0x01 && d[1] == 0xF4 && d[2] == 0xFF && d[3] == 0x06 && d[4] == 0x03 && d[5] == 0xE8);

    pack_position(d, 12971599, 77594566);
    assert(d[0] == 0x00 && d[1] == 0xC5 && d[2] == 0xEE && d[3] == 0x4F);
    assert(d[4] == 0x04 && d[5] == 0x9F && d[6] == 0xFF && d[7] == 0xC6);
    pack_position(d, -33868820, -151209297);
    assert(d[0] == 0xFD && d[1] == 0xFB && d[2] == 0x33 && d[3] == 0xEC);
    assert(d[4] == 0xF6 && d[5] == 0xFC && d[6] == 0xBA && d[7] == 0xAF);
    {
        int32_t lat = (int32_t)(((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3]);
        int32_t lon = (int32_t)(((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7]);
        assert(lat == -33868820 && lon == -151209297);
    }

    /* accelerometer scaling: 1024 counts = 1 g = 1000 mg, negative values keep sign */
    assert(accel_raw_to_mg(1024) == 1000);
    assert(accel_raw_to_mg(-512) == -500);
    assert(accel_raw_to_mg(0) == 0);

    printf("vhms_logic: all tests passed\n");
    return 0;
}
