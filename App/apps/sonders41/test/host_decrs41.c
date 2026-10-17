#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../decrs41.h"

static const uint8_t MASK[64] = {
    0x96,0x83,0x3E,0x51,0xB1,0x49,0x08,0x98,
    0x32,0x05,0x59,0x0E,0xF9,0x44,0xC6,0x26,
    0x21,0x60,0xC2,0xEA,0x79,0x5D,0x6D,0xA1,
    0x54,0x69,0x47,0x0C,0xDC,0xE8,0x5C,0xF1,
    0xF7,0x76,0x82,0x7F,0x07,0x99,0xA2,0x2C,
    0x93,0x7C,0x30,0x63,0xF5,0x10,0x2E,0x61,
    0xD0,0xBC,0xB4,0xB6,0x06,0xAA,0xF4,0x23,
    0x78,0x6E,0x3B,0xAE,0xBF,0x7B,0x4C,0xC1
};

static const uint32_t ARC[RS41_ARC_N] = {
    162000u, 95634u, 50530u, 25650u, 12875u, 6444u, 3223u,
    1611u, 806u, 403u, 201u, 101u, 50u, 25u
};

/* decrs41_init, then the tables the app reads from its assets. */
static void initDec(decrs41_t *d)
{
    decrs41_init(d);
    memcpy(d->arc, ARC, sizeof ARC);
    memcpy(d->mask, MASK, sizeof MASK);
}

static uint16_t crc16(const uint8_t *p, uint8_t len)
{
    uint16_t crc = 0xFFFFu;
    while (len--) {
        crc ^= (uint16_t)*p++ << 8;
        for (uint8_t b = 0; b < 8u; b++) {
            bool high = (crc & 0x8000u) != 0u;
            crc = (uint16_t)(crc << 1);
            if (high) crc = (uint16_t)(crc ^ (uint16_t)0x1021u);
        }
    }
    return crc;
}

static void put32(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

static void put16(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

static void finishBlock(uint8_t *f, uint16_t pos)
{
    uint8_t len = f[pos + 1u];
    uint16_t crc = crc16(f + pos + 2u, len);
    f[pos + 2u + len] = (uint8_t)crc;
    f[pos + 3u + len] = (uint8_t)(crc >> 8);
}

static void makeFrame(uint8_t *clear, uint8_t *wire)
{
    static const uint8_t sync[8] =
        { 0x86,0x35,0xF4,0x40,0x93,0xDF,0x1A,0x60 };
    uint8_t *status, *gps;
    memset(clear, 0, RS41_FRAME_LEN);
    memcpy(clear, sync, sizeof sync);

    status = clear + 0x39;
    status[0] = 0x79;
    status[1] = 40;
    status[2] = 0x34;
    status[3] = 0x12;
    memcpy(status + 4, "S1234567", 8);
    status[25] = 0;
    finishBlock(clear, 0x39);

    gps = clear + 0x112;
    gps[0] = 0x7B;
    gps[1] = 21;
    /* WGS84 ECEF for 48.8 N, 2.3 E at 1200 m, in centimetres. */
    put32(gps + 2, 420659420);
    put32(gps + 6, 16895428);
    put32(gps + 10, 477684060);
    /* ECEF velocity in cm/s: 5.2 m/s up, 8 m/s east, 3 m/s south. */
    put16(gps + 14, 536);
    put16(gps + 16, 822);
    put16(gps + 18, 194);
    gps[20] = 8;
    finishBlock(clear, 0x112);

    for (uint16_t i = 0; i < RS41_FRAME_LEN; i++)
        wire[i] = clear[i] ^ MASK[i & 63u];
}

static void loadWire(decrs41_t *d, const uint8_t *wire)
{
    static const uint8_t sync[8] =
        { 0x86,0x35,0xF4,0x40,0x93,0xDF,0x1A,0x60 };
    memcpy(d->sl[0].frm, wire, RS41_FRAME_LEN);
    memcpy(d->sl[0].frm, sync, sizeof sync);
    d->sl[0].state = 2u;               /* ST_DONE: a complete frame */
}

static void checkParse(void)
{
    uint8_t clear[RS41_FRAME_LEN], wire[RS41_FRAME_LEN];
    decrs41_t d;
    sonde_info_t info = {0};
    makeFrame(clear, wire);
    initDec(&d);
    loadWire(&d, wire);
    assert(decrs41_parse(&d, &info));
    assert(!strcmp(info.id, "S1234567"));
    assert((info.flags & (SONDE_HAS_ID | SONDE_HAS_POS | SONDE_HAS_ALT)) ==
           (SONDE_HAS_ID | SONDE_HAS_POS | SONDE_HAS_ALT));
    assert(info.latS > 175500 && info.latS < 175900);
    assert(info.lonS > 8100 && info.lonS < 8500);
    assert(info.alt_m > 1150 && info.alt_m < 1250);
    assert(info.vs_dm >= 51 && info.vs_dm <= 53);
    assert(info.sats == 8u);

    initDec(&d);
    wire[0x112 + 4] ^= 1u;
    loadWire(&d, wire);
    memset(&info, 0, sizeof info);
    assert(decrs41_parse(&d, &info));
    assert(info.flags == SONDE_HAS_ID);

    makeFrame(clear, wire);
    initDec(&d);
    wire[0x39 + 4] ^= 1u;
    loadWire(&d, wire);
    assert(!decrs41_parse(&d, &info));
}

static void feedBit(decrs41_t *d, uint8_t bit, bool inverted)
{
    uint16_t sample = (bit ^ inverted) ? 3200u : 800u;
    for (uint8_t i = 0; i < 4u; i++) decrs41_push(d, sample);
}

static void checkDemod(bool inverted)
{
    uint8_t clear[RS41_FRAME_LEN], wire[RS41_FRAME_LEN];
    decrs41_t d;
    sonde_info_t info = {0};
    makeFrame(clear, wire);
    initDec(&d);
    for (uint16_t i = 0; i < 320u; i++) feedBit(&d, i & 1u, inverted);
    for (uint16_t i = 0; i < RS41_FRAME_LEN; i++)
        for (uint8_t b = 0; b < 8u; b++)
            feedBit(&d, (uint8_t)((wire[i] >> b) & 1u), inverted);
    /* The last bit is read a sample or two after its end. */
    bool done = false;
    for (uint8_t i = 0; i < 8u; i++) done |= decrs41_push(&d, inverted ? 800u : 3200u);
    assert(done);
    assert(decrs41_parse(&d, &info));
    assert(!strcmp(info.id, "S1234567"));
    assert(info.flags & SONDE_HAS_POS);
}

int main(void)
{
    checkParse();
    checkDemod(false);
    checkDemod(true);
    puts("RS41 decoder tests passed");
    return 0;
}
